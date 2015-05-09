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
#include "replica.h"
#include "mutation.h"
#include "mutation_log.h"
#include "replica_stub.h"
#include <boost/filesystem.hpp>

#define __TITLE__ "Learn"

namespace dsn { namespace replication {

void replica::init_learn(uint64_t signature)
{
    check_hashed_access();

    dassert (status() == PS_POTENTIAL_SECONDARY, "");
        
    // at most one learning task running
    if (_potential_secondary_states.learning_round_is_running || !signature)
        return;

    if (signature != _potential_secondary_states.learning_signature)
    {
        _potential_secondary_states.cleanup(true);
        _potential_secondary_states.learning_signature = signature;
        _potential_secondary_states.LearningState = LearningWithoutPrepare;
        _prepare_list->reset(_app->last_committed_decree());
    }
    else
    {
        switch (_potential_secondary_states.LearningState)
        {
        case LearningSucceeded:
            notify_learn_completion();
            return;
        case LearningFailed:
            break;
        case LearningWithPrepare:
            if (_app->last_durable_decree() >= last_committed_decree())
            {
                _potential_secondary_states.LearningState = LearningSucceeded;
                notify_learn_completion();
                return;
            }
            break;
        case LearningWithoutPrepare:
            break;
        default:
            dassert (false, "");
        }
    }
        
    _potential_secondary_states.learning_round_is_running = true;

    std::shared_ptr<learn_request> request(new learn_request);
    request->gpid = get_gpid();
    request->last_committed_decree_in_app = _app->last_committed_decree();
    request->last_committed_decree_in_prepare_list = _prepare_list->last_committed_decree();
    request->learner = primary_address();
    request->signature = _potential_secondary_states.learning_signature;
    _app->prepare_learning_request(request->app_specific_learn_request);

    _potential_secondary_states.learning_task = rpc::call_typed(
        _config.primary,
        RPC_LEARN,
        request,        
        this,
        &replica::on_learn_reply,
        gpid_to_hash(get_gpid())
        );

    ddebug(
        "%s: init_learn with lastAppC/DDecree = <%llu,%llu>, lastCDecree = %llu, learnState = %s",
        name(),
        _app->last_committed_decree(),
        _app->last_durable_decree(),
        last_committed_decree(),
        enum_to_string(_potential_secondary_states.LearningState)
        );
}

void replica::on_learn(const learn_request& request, __out_param learn_response& response)
{
    check_hashed_access();

    if (PS_PRIMARY  != status())
    {
        response.err = ERR_INVALID_STATE;
        return;
    }
        
    if (request.last_committed_decree_in_app > last_committed_decree())
    {
        ddebug(
            "%s: on_learn %s:%u, learner state is lost due to DDD, with its appCommittedDecree = %llu vs localCommitedDecree %llu",
            name(),
            request.learner.name.c_str(), static_cast<int>(request.learner.port),
            request.last_committed_decree_in_app,
            last_committed_decree()
            );
        ((learn_request&)request).last_committed_decree_in_app = 0;
    }

    _primary_states.get_replica_config(request.learner, response.config);

    auto it = _primary_states.learners.find(request.learner);
    if (it == _primary_states.learners.end())
    {
        response.err = (response.config.status == PS_SECONDARY ? ERR_SUCCESS : ERR_OBJECT_NOT_FOUND);
        return;
    }
    else if (it->second.signature != request.signature)
    {
        response.err = ERR_OBJECT_NOT_FOUND;
        return;
    }

    ddebug(
        "%s: on_learn %s:%u with its appCommittedDecree = %llu vs localCommitedDecree %llu",
        name(),
        request.learner.name.c_str(), static_cast<int>(request.learner.port),
        request.last_committed_decree_in_app,
        last_committed_decree()
        );
    
    response.prepare_start_decree = invalid_decree;
    response.commit_decree = last_committed_decree();
    response.err = ERR_SUCCESS; 

    if (request.last_committed_decree_in_app + _options.staleness_for_start_prepare_for_potential_secondary >= last_committed_decree())
    {
        if (it->second.prepare_start_decree == invalid_decree)
        {
            it->second.prepare_start_decree = last_committed_decree() + 1;

            cleanup_preparing_mutations(true);
            replay_prepare_list();

            ddebug(
                "%s: on_learn with prepare_start_decree = %llu for %s:%u",
                name(),
                last_committed_decree() + 1,
                request.learner.name.c_str(), static_cast<int>(request.learner.port)
            );
        }

        response.prepare_start_decree = it->second.prepare_start_decree;
    }
    else
    {
        it->second.prepare_start_decree = invalid_decree;
    }

    decree decree = request.last_committed_decree_in_app + 1;
    response.err = _app->get_learn_state(decree, request.app_specific_learn_request, response.state);
        
    response.base_local_dir = _dir;
    for (auto itr = response.state.files.begin(); itr != response.state.files.end(); ++itr)            
        *itr = itr->substr(_dir.length()); 
}

void replica::on_learn_reply(error_code err, std::shared_ptr<learn_request>& req, std::shared_ptr<learn_response>& resp)
{
    check_hashed_access();

    dassert (PS_POTENTIAL_SECONDARY == status(), "");
    dassert (req->signature == _potential_secondary_states.learning_signature, "");

    if (resp == nullptr)
    {
        handle_learning_error(ERR_TIMEOUT);
        return;
    }

    ddebug(
        "%s: on_learn_reply with err = 0x%x, prepare_start_decree = %llu, current learnState = %s",
        name(), resp->err, resp->prepare_start_decree, enum_to_string(_potential_secondary_states.LearningState)
        );

    if (resp->err != ERR_SUCCESS)
    {
        handle_learning_error(resp->err);
        return;
    }

    if (resp->config.ballot > get_ballot())
    {
        update_local_configuration(resp->config);
    }

    if (status() != PS_POTENTIAL_SECONDARY)
    {
        return;
    }

    if (resp->prepare_start_decree != invalid_decree && _potential_secondary_states.LearningState == LearningWithoutPrepare)
    {
        _potential_secondary_states.LearningState = LearningWithPrepare;
        _prepare_list->reset(resp->prepare_start_decree - 1);
    }

    _potential_secondary_states.learn_remote_files_task = tasking::enqueue(
        LPC_LEARN_REMOTE_DELTA_FILES,
        this,
        std::bind(&replica::on_learn_remote_state, this, resp)        
        );
}

void replica::on_learn_remote_state(std::shared_ptr<learn_response> resp)
{
    int err = ERR_SUCCESS;
    
    //
    // TODO: copy files using data bus service instead
    //    
    learn_state localState;
    localState.meta = resp->state.meta;

    end_point& server = resp->config.primary;
                
    if (!resp->state.files.empty())
    {
        file::copy_remote_files(server, resp->base_local_dir, resp->state.files, _dir, true, LPC_AIO_TEST, nullptr, nullptr);
    }

    if (err == ERR_SUCCESS)
    {
        for (auto itr = resp->state.files.begin(); itr != resp->state.files.end(); ++itr)
        {
            std::string file;
            if (dir().back() == '/' || itr->front() == '/')
                file = dir() + *itr;
            else 
                file = dir() + '/' + *itr;

            localState.files.push_back(file);
        }
                
         // the only place where there is non-in-partition-thread update  
        decree oldDecree = _app->last_committed_decree();

        err = _app->apply_learn_state(resp->state);

        ddebug(
                "%s: learning %d files to %s, err = %x, "
                "appCommit(%llu => %llu), durable(%llu), remoteC(%llu), prepStart(%llu), state(%s)",
                name(),
                resp->state.files.size(), _dir.c_str(), err,
                oldDecree, _app->last_committed_decree(),
                _app->last_durable_decree(),                
                resp->commit_decree,
                resp->prepare_start_decree,
                enum_to_string(_potential_secondary_states.LearningState)
                );

        if (err == ERR_SUCCESS && _app->last_committed_decree() >= resp->commit_decree)
        {
            err = _app->flush(true);
            if (err == ERR_SUCCESS)
            {
                dassert (_app->last_committed_decree() == _app->last_durable_decree(), "");
            }
        }
    } 
    else 
    {
        derror(
                "%s: Transfer %d files to %s failed, err = %d",
                name(),
                resp->state.files.size(), _dir.c_str(), err);
    }    

    _potential_secondary_states.learn_remote_files_completed_task = tasking::enqueue(
        LPC_LEARN_REMOTE_DELTA_FILES_COMPLETED,
        this,
        std::bind(&replica::on_learn_remote_state_completed, this, err),
        gpid_to_hash(get_gpid())
        );
}

void replica::on_learn_remote_state_completed(int err)
{
    check_hashed_access();
    
    if (PS_POTENTIAL_SECONDARY != status())
        return;

    _potential_secondary_states.learning_round_is_running = false;

    if (err != ERR_SUCCESS)
    {
        handle_learning_error(err);
    }
    else
    {
        // continue
        init_learn(_potential_secondary_states.learning_signature);
    }
}

void replica::handle_learning_error(int err)
{
    check_hashed_access();

    dwarn(
        "%s: learning failed with err = 0x%X, LastCommitted = %lld",
        name(),
        err,
        _app->last_committed_decree()
        );

    _potential_secondary_states.cleanup(true);
    _potential_secondary_states.LearningState = LearningFailed;

    update_local_configuration_with_no_ballot_change(PS_ERROR);
}

void replica::handle_learning_succeeded_on_primary(const end_point& node, uint64_t learnSignature)
{
    auto it = _primary_states.learners.find(node);
    if (it != _primary_states.learners.end() && it->second.signature == learnSignature)
        upgrade_to_secondary_on_primary(node);
}

void replica::notify_learn_completion()
{
    group_check_response report;
    report.gpid = get_gpid();
    report.err = ERR_SUCCESS;
    report.last_committed_decree_in_app = _app->last_committed_decree();
    report.last_committed_decree_in_prepare_list = last_committed_decree();
    report.learner_signature = _potential_secondary_states.learning_signature;
    report.learner_status_ = _potential_secondary_states.LearningState;
    report.node = primary_address();
    
    rpc::call_one_way_typed(_config.primary, RPC_LEARN_COMPLETITION_NOTIFY, report, gpid_to_hash(get_gpid()));
}

void replica::on_learn_completion_notification(const group_check_response& report)
{
    check_hashed_access();
    if (status() != PS_PRIMARY)
        return;

    if (report.learner_status_ == LearningSucceeded)
    {
        handle_learning_succeeded_on_primary(report.node, report.learner_signature);
    }
}

void replica::on_add_learner(const group_check_request& request)
{
    if (request.config.ballot < get_ballot())
        return;

    if (request.config.ballot > get_ballot()
        || is_same_ballot_status_change_allowed(status(), request.config.status))
    {
        update_local_configuration(request.config, true);
        dassert(PS_POTENTIAL_SECONDARY == status(), "");
        init_learn(request.learner_signature);
    }
}

}} // namespace
