/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     replica configuration management
 *
 * Revision history:
 *     Mar., 2015, @imzhenyu (Zhenyu Guo), first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */

#include "replica.h"
#include "mutation.h"
#include "mutation_log.h"
#include "replica_stub.h"
#include "replication_app_base.h"

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "replica.config"

namespace dsn { namespace replication {

void replica::on_config_proposal(configuration_update_request& proposal)
{
    check_hashed_access();

    ddebug(
        "%s: process config proposal %s for %s",
        name(),
        enum_to_string(proposal.type),
        proposal.node.to_string()
        );

    if (proposal.config.ballot < get_ballot())
    {
        dwarn(
            "%s: on_config_proposal out-dated, %" PRId64 " vs %" PRId64,
            name(),
            proposal.config.ballot,
            get_ballot()
            );
        return;
    }   

    if (_primary_states.reconfiguration_task != nullptr)
    {
        dinfo(
            "%s: reconfiguration on the way, skip the incoming proposal",
            name()
            );
        return;
    }

    if (proposal.config.ballot > get_ballot())
    {
        if (!update_configuration(proposal.config))
        {
            // is closing or update failed
            return;
        }
    }
    
    switch (proposal.type)
    {
    case config_type::CT_ASSIGN_PRIMARY:
    case config_type::CT_UPGRADE_TO_PRIMARY:
        assign_primary(proposal);
        break;
    case config_type::CT_ADD_SECONDARY:
    case config_type::CT_ADD_SECONDARY_FOR_LB:
        add_potential_secondary(proposal);
        break;
    case config_type::CT_DOWNGRADE_TO_SECONDARY:
        downgrade_to_secondary_on_primary(proposal);
        break;
    case config_type::CT_DOWNGRADE_TO_INACTIVE:
        downgrade_to_inactive_on_primary(proposal);
        break;
    case config_type::CT_REMOVE:
        remove(proposal);
        break;
    default:
        dassert (false, "");
    }
}

void replica::assign_primary(configuration_update_request& proposal)
{
    dassert(proposal.node == _stub->_primary_address, "");

    if (status() == partition_status::PS_PRIMARY)
    {
        dwarn(
            "%s: invalid assgin primary proposal as the node is in %s",
            name(),
            enum_to_string(status()));
        return;
    }

    if (proposal.type == config_type::CT_UPGRADE_TO_PRIMARY
        && (status() != partition_status::PS_SECONDARY || _secondary_states.checkpoint_is_running))
    {
        dwarn(
            "%s: invalid upgrade to primary proposal as the node is in %s or during checkpointing",
            name(),
            enum_to_string(status()));

        // TODO: tell meta server so new primary is built more quickly
        return;
    }

    proposal.config.primary = _stub->_primary_address;
    replica_helper::remove_node(_stub->_primary_address, proposal.config.secondaries);

    update_configuration_on_meta_server(proposal.type, proposal.node, proposal.config);
}

// run on primary to send ADD_LEARNER request to candidate replica server
void replica::add_potential_secondary(configuration_update_request& proposal)
{
    if (status() != partition_status::PS_PRIMARY)
    {
        dwarn("%s: ignore add secondary proposal for invalid state, state = %s", name(), enum_to_string(status()));
        return;
    }

    dassert (proposal.config.ballot == get_ballot(), "");
    dassert (proposal.config.pid == _primary_states.membership.pid, "");
    dassert (proposal.config.primary == _primary_states.membership.primary, "");
    dassert (proposal.config.secondaries == _primary_states.membership.secondaries, "");
    dassert (!_primary_states.check_exist(proposal.node, partition_status::PS_PRIMARY), "");
    dassert (!_primary_states.check_exist(proposal.node, partition_status::PS_SECONDARY), "");

    int potential_secondaries_count = _primary_states.membership.secondaries.size() + _primary_states.learners.size();
    if (potential_secondaries_count >= _primary_states.membership.max_replica_count - 1)
    {
        if (proposal.type == config_type::CT_ADD_SECONDARY)
        {
            if (_primary_states.learners.find(proposal.node) == _primary_states.learners.end())
            {
                ddebug("%s: already have enough secondaries or potential secondaries, ignore new potential secondary proposal", name());
                return;
            }
        }
        else if (proposal.type == config_type::CT_ADD_SECONDARY_FOR_LB)
        {
            if (potential_secondaries_count >= _primary_states.membership.max_replica_count)
            {
                ddebug("%s: only allow one extra (potential) secondary, ingnore new potential secondary proposal", name());
                return;
            }
            else
            {
                ddebug("%s: add a new secondary(%s) for future load balancer", name(), proposal.node.to_string());
            }
        }
        else
        {
            dassert(false, "");
        }
    }

    remote_learner_state state;
    state.prepare_start_decree = invalid_decree;
    state.timeout_task = nullptr; // TODO: add timer for learner task

    auto it = _primary_states.learners.find(proposal.node);
    if (it != _primary_states.learners.end())
    {
        state.signature = it->second.signature;
    }
    else
    {
        state.signature = ++_primary_states.next_learning_version;
        _primary_states.learners[proposal.node] = state;
        _primary_states.statuses[proposal.node] = partition_status::PS_POTENTIAL_SECONDARY;
    }

    group_check_request request;
    request.app = _app_info;
    request.node = proposal.node;
    _primary_states.get_replica_config(partition_status::PS_POTENTIAL_SECONDARY, request.config, state.signature);
    request.last_committed_decree = last_committed_decree();

    ddebug(
        "%s: call one way %s to start learning with signature [%" PRIx64"]",
        name(),
        proposal.node.to_string(),
        state.signature
    );

    rpc::call_one_way_typed(proposal.node, RPC_LEARN_ADD_LEARNER, request, gpid_to_hash(get_gpid()));
}

void replica::upgrade_to_secondary_on_primary(::dsn::rpc_address node)
{
    ddebug(
        "%s: upgrade potential secondary %s to secondary",
        name(),
        node.to_string()
    );

    partition_configuration newConfig = _primary_states.membership;

    // add secondary
    newConfig.secondaries.push_back(node);

    update_configuration_on_meta_server(config_type::CT_UPGRADE_TO_SECONDARY, node, newConfig);
}

void replica::downgrade_to_secondary_on_primary(configuration_update_request& proposal)
{
    if (proposal.config.ballot != get_ballot() || status() != partition_status::PS_PRIMARY)
        return;

    dassert (proposal.config.pid == _primary_states.membership.pid, "");
    dassert (proposal.config.primary == _primary_states.membership.primary, "");
    dassert (proposal.config.secondaries == _primary_states.membership.secondaries, "");
    dassert (proposal.node == proposal.config.primary, "");

    proposal.config.primary.set_invalid();
    proposal.config.secondaries.push_back(proposal.node);

    update_configuration_on_meta_server(config_type::CT_DOWNGRADE_TO_SECONDARY, proposal.node, proposal.config);
}


void replica::downgrade_to_inactive_on_primary(configuration_update_request& proposal)
{
    if (proposal.config.ballot != get_ballot() || status() != partition_status::PS_PRIMARY)
        return;

    dassert (proposal.config.pid == _primary_states.membership.pid, "");
    dassert (proposal.config.primary == _primary_states.membership.primary, "");
    dassert (proposal.config.secondaries == _primary_states.membership.secondaries, "");

    if (proposal.node == proposal.config.primary)
    {
        proposal.config.primary.set_invalid();
    }
    else
    {
        auto rt = replica_helper::remove_node(proposal.node, proposal.config.secondaries);
        dassert (rt, "");
    }

    update_configuration_on_meta_server(config_type::CT_DOWNGRADE_TO_INACTIVE, proposal.node, proposal.config);
}

void replica::remove(configuration_update_request& proposal)
{
    if (proposal.config.ballot != get_ballot() || status() != partition_status::PS_PRIMARY)
        return;

    dassert (proposal.config.pid == _primary_states.membership.pid, "");
    dassert (proposal.config.primary == _primary_states.membership.primary, "");
    dassert (proposal.config.secondaries == _primary_states.membership.secondaries, "");

    auto st = _primary_states.get_node_status(proposal.node);

    switch (st)
    {
    case partition_status::PS_PRIMARY:
        dassert (proposal.config.primary == proposal.node, "");
        proposal.config.primary.set_invalid();
        break;
    case partition_status::PS_SECONDARY:
        {
        auto rt = replica_helper::remove_node(proposal.node, proposal.config.secondaries);
        dassert (rt, "");
        }
        break;
    case partition_status::PS_POTENTIAL_SECONDARY:
        break;
    default:
        break;
    }

    update_configuration_on_meta_server(config_type::CT_REMOVE, proposal.node, proposal.config);
}

// from primary
void replica::on_remove(const replica_configuration& request)
{ 
    if (request.ballot < get_ballot())
        return;

    //
    // - meta-server requires primary r1 to remove this secondary r2
    // - primary update config from {3,r1,[r2,r3]} to {4,r1,[r3]}
    // - primary send one way RPC_REMOVE_REPLICA to r2, but this message is delay by network
    // - meta-server requires primary r1 to add new secondary on r2 again (though this case would not occur generally)
    // - primary send RPC_LEARN_ADD_LEARNER to r2 with config of {4,r1,[r3]}, then r2 start to learn
    // - when r2 is on learning, the remove request is arrived, with the same ballot
    // - here we ignore the lately arrived remove request, which is proper
    //
    if (request.ballot == get_ballot() && partition_status::PS_POTENTIAL_SECONDARY == status())
    {
        dwarn("this implies that a config proposal request (e.g. add secondary) "
              "with the same ballot arrived before this remove request, "
              "current status is %s", enum_to_string(status()));
        return;
    }

    dassert (request.status == partition_status::PS_INACTIVE, "");
    update_local_configuration(request);
}

void replica::update_configuration_on_meta_server(config_type::type type, ::dsn::rpc_address node, partition_configuration& newConfig)
{
    newConfig.last_committed_decree = last_committed_decree();

    if (type != config_type::CT_ASSIGN_PRIMARY && type != config_type::CT_UPGRADE_TO_PRIMARY)
    {
        dassert (status() == partition_status::PS_PRIMARY, "");
        dassert (newConfig.ballot == _primary_states.membership.ballot, "");
    }

    // disable 2pc during reconfiguration
    // it is possible to do this only for config_type::CT_DOWNGRADE_TO_SECONDARY,
    // but we choose to disable 2pc during all reconfiguration types
    // for simplicity at the cost of certain write throughput
    update_local_configuration_with_no_ballot_change(partition_status::PS_INACTIVE);
    set_inactive_state_transient(true);

    dsn_message_t msg = dsn_msg_create_request(RPC_CM_UPDATE_PARTITION_CONFIGURATION, 0, 0);
    
    std::shared_ptr<configuration_update_request> request(new configuration_update_request);
    request->info = _app_info;
    request->config = newConfig;
    request->config.ballot++;
    request->type = type;
    request->node = node;

    ::dsn::marshall(msg, *request);

    if (nullptr != _primary_states.reconfiguration_task)
    {
        _primary_states.reconfiguration_task->cancel(true);
    }

    ddebug("%s: send update configuration request to meta server, ballot = %" PRId64 ", type = %s, node = %s",
           name(), request->config.ballot, enum_to_string(request->type), request->node.to_string());

    rpc_address target(_stub->_failure_detector->get_servers());
    _primary_states.reconfiguration_task = rpc::call(
        target,
        msg,        
        this,
        [=](error_code err, dsn_message_t reqmsg, dsn_message_t response)
        {
            on_update_configuration_on_meta_server_reply(err, reqmsg, response, request);
        },
        gpid_to_hash(get_gpid())
        );
}

void replica::on_update_configuration_on_meta_server_reply(error_code err, dsn_message_t request, dsn_message_t response, std::shared_ptr<configuration_update_request> req)
{
    check_hashed_access();

    if (partition_status::PS_INACTIVE != status() || _stub->is_connected() == false)
    {
        _primary_states.reconfiguration_task = nullptr;
        err.end_tracking();
        return;
    }

    configuration_update_response resp;
    if (err == ERR_OK)
    {
        ::dsn::unmarshall(response, resp);
        err = resp.err;
    }

    if (err != ERR_OK)
    {
        ddebug(
            "%s: update configuration reply with err %s, request ballot %" PRId64,
            name(),
            err.to_string(),
            req->config.ballot
            );

        if (err != ERR_INVALID_VERSION)
        {
            rpc_address target(_stub->_failure_detector->get_servers());
            _primary_states.reconfiguration_task = rpc::create_rpc_response_task(
                request,
                this,
                [this, req](error_code err, dsn_message_t request, dsn_message_t response)
                {
                    on_update_configuration_on_meta_server_reply(err, request, response, std::move(req));
                },
                gpid_to_hash(get_gpid())
            );
            //when the rpc call timeout, we would delay to do the recall
            _primary_states.reconfiguration_task->set_delay(1000);
            dsn_rpc_call(target.c_addr(), _primary_states.reconfiguration_task->native_handle());
            return;
        }
    }

    ddebug(
        "%s: update configuration %s, reply with err %s, ballot %" PRId64 ", local ballot %" PRId64 ", local status %s",
        name(),
        enum_to_string(req->type),
        resp.err.to_string(),
        resp.config.ballot,
        get_ballot(),
        enum_to_string(status())
        );
    
    if (resp.config.ballot < get_ballot())
    {
        _primary_states.reconfiguration_task = nullptr;
        return;
    }        
    
    // post-update work items?
    if (resp.err == ERR_OK)
    {        
        dassert (req->config.pid == resp.config.pid, "");
        dassert (req->config.primary == resp.config.primary, "");
        dassert (req->config.secondaries == resp.config.secondaries, "");

        switch (req->type)
        {        
        case config_type::CT_UPGRADE_TO_PRIMARY:
            _primary_states.last_prepare_decree_on_new_primary = _prepare_list->max_decree();
            break;
        case config_type::CT_ASSIGN_PRIMARY:
            _primary_states.last_prepare_decree_on_new_primary = 0;
            break;
        case config_type::CT_DOWNGRADE_TO_SECONDARY:
        case config_type::CT_DOWNGRADE_TO_INACTIVE:
        case config_type::CT_UPGRADE_TO_SECONDARY:
            break;
        case config_type::CT_REMOVE:
            if (req->node != _stub->_primary_address)
            {
                replica_configuration rconfig;
                replica_helper::get_replica_config(resp.config, req->node, rconfig);
                rpc::call_one_way_typed(req->node, RPC_REMOVE_REPLICA, rconfig, gpid_to_hash(get_gpid()));
            }
            break;
        default:
            dassert (false, "");
        }
    }
    
    update_configuration(resp.config);
    _primary_states.reconfiguration_task = nullptr;
}

bool replica::update_configuration(const partition_configuration& config)
{
    dassert (config.ballot >= get_ballot(), "");
    
    replica_configuration rconfig;
    replica_helper::get_replica_config(config, _stub->_primary_address, rconfig);

    if (rconfig.status == partition_status::PS_PRIMARY &&
        (rconfig.ballot > get_ballot() || status() != partition_status::PS_PRIMARY)
        )
    {
        _primary_states.reset_membership(config, config.primary != _stub->_primary_address);
    }

    if (config.ballot > get_ballot() ||
        is_same_ballot_status_change_allowed(status(), rconfig.status)
        )
    {
        return update_local_configuration(rconfig, true);
    }
    else
        return false;
}

bool replica::is_same_ballot_status_change_allowed(partition_status::type olds, partition_status::type news)
{
    return
        // add learner
        (olds == partition_status::PS_INACTIVE && news == partition_status::PS_POTENTIAL_SECONDARY)

        // learner ready for secondary
        || (olds == partition_status::PS_POTENTIAL_SECONDARY && news == partition_status::PS_SECONDARY)

        // meta server come back
        || (olds == partition_status::PS_INACTIVE && news == partition_status::PS_SECONDARY && _inactive_is_transient)

        // meta server come back
        || (olds == partition_status::PS_INACTIVE && news == partition_status::PS_PRIMARY && _inactive_is_transient)

        // no change
        || (olds == news)
        ;
}

bool replica::update_local_configuration(const replica_configuration& config, bool same_ballot/* = false*/)
{
    dassert(config.ballot > get_ballot()
        || (same_ballot && config.ballot == get_ballot()), "");
    dassert (config.pid == get_gpid(), "");

    partition_status::type old_status = status();
    ballot old_ballot = get_ballot();

    // skip unncessary configuration change
    if (old_status == config.status && old_ballot == config.ballot)
        return true;

    // skip invalid change
    // but do not disable transitions to partition_status::PS_ERROR as errors
    // must be handled immmediately
    switch (old_status)
    {
    case partition_status::PS_ERROR:
        {
            dwarn(
                "%s: status change from %s @ %" PRId64 " to %s @ %" PRId64 " is not allowed",
                name(),
                enum_to_string(old_status),
                old_ballot,
                enum_to_string(config.status),
                config.ballot
                );
            return false;
        }
        break;
    case partition_status::PS_INACTIVE:
        if ((config.status == partition_status::PS_PRIMARY || config.status == partition_status::PS_SECONDARY)
            && !_inactive_is_transient)
        {
            dwarn(
                "%s: status change from %s @ %" PRId64 " to %s @ %" PRId64 " is not allowed when inactive state is not transient",
                name(),
                enum_to_string(old_status),
                old_ballot,
                enum_to_string(config.status),
                config.ballot
                );
            return false;
        }
        break;
    case partition_status::PS_POTENTIAL_SECONDARY:
        if (config.status == partition_status::PS_INACTIVE)
        {
            if (!_potential_secondary_states.cleanup(false))
            {
                dwarn(
                    "%s: status change from %s @ %" PRId64 " to %s @ %" PRId64 " is not allowed coz learning remote state is still running",
                    name(),
                    enum_to_string(old_status),
                    old_ballot,
                    enum_to_string(config.status),
                    config.ballot
                    );
                return false;
            }
        }
        break;
    case partition_status::PS_SECONDARY:
        if (config.status != partition_status::PS_SECONDARY && config.status != partition_status::PS_ERROR)
        {
            if (!_secondary_states.cleanup(false))
            {
                dsn_task_t native_handle;
                if (_secondary_states.checkpoint_task)
                    native_handle = _secondary_states.checkpoint_task->native_handle();
                else if (_secondary_states.checkpoint_completed_task)
                    native_handle = _secondary_states.checkpoint_completed_task->native_handle();
                else if (_secondary_states.catchup_with_private_log_task)
                    native_handle = _secondary_states.catchup_with_private_log_task->native_handle();
                else
                    native_handle = nullptr;

                dwarn(
                    "%s: status change from %s @ %" PRId64 " to %s @ %" PRId64 " is not allowed coz checkpointing %p is still running",
                    name(),
                    enum_to_string(old_status),
                    old_ballot,
                    enum_to_string(config.status),
                    config.ballot,
                    native_handle
                    );
                return false;
            }
        }
        break;
    default:
        break;
    }

    bool r = false;
    uint64_t oldTs = _last_config_change_time_ms;
    _config = config;
    _last_config_change_time_ms = now_ms();
    dassert (max_prepared_decree() >= last_committed_decree(), "");
    
    switch (old_status)
    {
    case partition_status::PS_PRIMARY:
        cleanup_preparing_mutations(false);
        switch (config.status)
        {
        case partition_status::PS_PRIMARY:
            replay_prepare_list();
            break;
        case partition_status::PS_INACTIVE:
            _primary_states.cleanup(old_ballot != config.ballot);
            break;
        case partition_status::PS_SECONDARY:
        case partition_status::PS_ERROR:
            _primary_states.cleanup(true);
            break;
        case partition_status::PS_POTENTIAL_SECONDARY:
            dassert (false, "invalid execution path");
            break;
        default:
            dassert (false, "invalid execution path");
        }        
        break;
    case partition_status::PS_SECONDARY:
        cleanup_preparing_mutations(false);
        switch (config.status)
        {
        case partition_status::PS_PRIMARY:
            init_group_check();            
            replay_prepare_list();
            break;
        case partition_status::PS_SECONDARY:
            break;
        case partition_status::PS_POTENTIAL_SECONDARY:
            // prevent further 2pc
            // wait next group check or explicit learn for real learning
            _potential_secondary_states.learning_status = learner_status::LearningWithoutPrepare;
            break;
        case partition_status::PS_INACTIVE:
            break;
        case partition_status::PS_ERROR:
            // _secondary_states.cleanup(true); => do it in close as it may block
            break;
        default:
            dassert (false, "invalid execution path");
        }
        break;
    case partition_status::PS_POTENTIAL_SECONDARY:
        switch (config.status)
        {
        case partition_status::PS_PRIMARY:
            dassert (false, "invalid execution path");
            break;
        case partition_status::PS_SECONDARY:
            _prepare_list->truncate(_app->last_committed_decree());            

            // using force cleanup now as all tasks must be done already
            r = _potential_secondary_states.cleanup(true);
            dassert(r, "%s: potential secondary context cleanup failed", name());

            check_state_completeness();
            break;
        case partition_status::PS_POTENTIAL_SECONDARY:
            break;
        case partition_status::PS_INACTIVE:
            break;
        case partition_status::PS_ERROR:
            _prepare_list->reset(_app->last_committed_decree());
            _potential_secondary_states.cleanup(false);
            // => do this in close as it may block
            // r = _potential_secondary_states.cleanup(true);
            // dassert(r, "%s: potential secondary context cleanup failed", name());
            break;
        default:
            dassert (false, "invalid execution path");
        }
        break;
    case partition_status::PS_INACTIVE:
        switch (config.status)
        {
        case partition_status::PS_PRIMARY:
            dassert (_inactive_is_transient, "must be in transient state for being primary next");
            _inactive_is_transient = false;
            init_group_check();
            replay_prepare_list();
            break;
        case partition_status::PS_SECONDARY:
            dassert(_inactive_is_transient, "must be in transient state for being secondary next");
            _inactive_is_transient = false;
            break;
        case partition_status::PS_POTENTIAL_SECONDARY:
            _inactive_is_transient = false;
            break;
        case partition_status::PS_INACTIVE:
            break;
        case partition_status::PS_ERROR:
            // => do this in close as it may block
            // if (_inactive_is_transient)
            // {
            //    _secondary_states.cleanup(true);
            // }

            if (_inactive_is_transient)
            {
                _primary_states.cleanup(true);
                _secondary_states.cleanup(false);
            }
            _inactive_is_transient = false;
            break;
        default:
            dassert (false, "invalid execution path");
        }
        break;
    case partition_status::PS_ERROR:
        switch (config.status)
        {
        case partition_status::PS_PRIMARY:
            dassert (false, "invalid execution path");
            break;
        case partition_status::PS_SECONDARY:
            dassert (false, "invalid execution path");
            break;
        case partition_status::PS_POTENTIAL_SECONDARY:
            dassert(false, "invalid execution path");
            break;
        case partition_status::PS_INACTIVE:
            dassert (false, "invalid execution path");
            break;
        case partition_status::PS_ERROR:
            break;
        default:
            dassert (false, "invalid execution path");
        }
        break;
    default:
        dassert (false, "invalid execution path");
    }

    ddebug(
        "%s: status change %s @ %" PRId64 " => %s @ %" PRId64 ", pre(%" PRId64 ", %" PRId64 "), app(%" PRId64 ", %" PRId64 "), duration = %" PRIu64 " ms",
        name(),
        enum_to_string(old_status),
        old_ballot,
        enum_to_string(status()),
        get_ballot(),
        _prepare_list->max_decree(),
        _prepare_list->last_committed_decree(),
        _app->last_committed_decree(),
        _app->last_durable_decree(),
        _last_config_change_time_ms - oldTs
        );

    if (status() != old_status)
    {
        bool is_closing = (status() == partition_status::PS_ERROR || (status() == partition_status::PS_INACTIVE && get_ballot() > old_ballot));
        _stub->notify_replica_state_update(config, is_closing);

        if (is_closing)
        {
            ddebug("%s: being close ...", name());
            _stub->begin_close_replica(this);
            return false;
        }
    }
    else
    {
        _stub->notify_replica_state_update(config, false);
    }

    // start pending mutations if necessary
    if (status() == partition_status::PS_PRIMARY)
    {
        mutation_ptr next = _primary_states.write_queue.check_possible_work(
            static_cast<int>(_prepare_list->max_decree() - last_committed_decree())
            );
        if (next)
        {
            init_prepare(next);
        }
    }

    return true;
}

bool replica::update_local_configuration_with_no_ballot_change(partition_status::type s)
{
    if (status() == s)
        return false;

    auto config = _config;
    config.status = s;
    return update_local_configuration(config, true);
}

void replica::on_config_sync(const partition_configuration& config)
{
    ddebug("%s: configuration sync", name());

    // no outdated update
    if (config.ballot < get_ballot())
        return;

    if (status() == partition_status::PS_PRIMARY || nullptr != _primary_states.reconfiguration_task)
    {
        // nothing to do as primary always holds the truth
    }
    else
    {
        update_configuration(config);

        if (status() == partition_status::PS_INACTIVE && !_inactive_is_transient)
        {
            if (config.primary == _stub->_primary_address // dead primary
                || config.primary.is_invalid() // primary is dead (otherwise let primary remove this)
                )
            {
                _stub->remove_replica_on_meta_server(_app_info, config);
            }
        }
    }
}

void replica::replay_prepare_list()
{
    decree start = last_committed_decree() + 1;
    decree end = _prepare_list->max_decree();

    ddebug(
            "%s: replay prepare list from %" PRId64 " to %" PRId64 ", ballot = %" PRId64,
            name(),
            start,
            end,
            get_ballot()
            );

    for (decree decree = start; decree <= end; decree++)
    {
        mutation_ptr old = _prepare_list->get_mutation_by_decree(decree);
        mutation_ptr mu = new_mutation(decree);

        if (old != nullptr)
        {
            dinfo("copy mutation from mutation_tid=%" PRIu64 " to mutation_tid=%" PRIu64,
                  old->tid(), mu->tid());
            mu->copy_from(old);
        }
        else
        {
            mu->add_client_request(RPC_REPLICATION_WRITE_EMPTY, nullptr);

            ddebug(
                "%s: emit empty mutation %s with mutation_tid=%" PRIu64 " when replay prepare list",
                name(),
                mu->name(),
                mu->tid()
                );
        }

        init_prepare(mu);
    }
}

}} // namespace
