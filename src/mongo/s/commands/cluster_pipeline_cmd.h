/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/s/query/cluster_aggregate.h"

namespace mongo {
namespace {

template <typename Impl>
class ClusterPipelineCommandBase final : public Command {
public:
    ClusterPipelineCommandBase() : Command(Impl::kName) {}

    const std::set<std::string>& apiVersions() const {
        return Impl::getApiVersions();
    }

    /**
     * It's not known until after parsing whether or not an aggregation command is an explain
     * request, because it might include the `explain: true` field (ie. aggregation explains do not
     * need to arrive via the `explain` command). Therefore even parsing of regular aggregation
     * commands needs to be able to handle the explain case.
     *
     * As a result, aggregation command parsing is done in parseForExplain():
     *
     * - To parse a regular aggregation command, call parseForExplain() with `explainVerbosity` of
     *   boost::none.
     *
     * - To parse an aggregation command as the sub-command in an `explain` command, call
     *   parseForExplain() with `explainVerbosity` set to the desired verbosity.
     */
    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        return parseForExplain(opCtx, opMsgRequest, boost::none);
    }

    std::unique_ptr<CommandInvocation> parseForExplain(
        OperationContext* opCtx,
        const OpMsgRequest& opMsgRequest,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity) override {
        const auto aggregationRequest =
            Impl::parseAggregationRequest(opCtx,
                                          opMsgRequest,
                                          explainVerbosity,
                                          APIParameters::get(opCtx).getAPIStrict().value_or(false));

        auto privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(AuthorizationSession::get(opCtx->getClient()),
                                            aggregationRequest.getNamespace(),
                                            aggregationRequest,
                                            true));

        return std::make_unique<Invocation>(
            this, opMsgRequest, std::move(aggregationRequest), std::move(privileges));
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd,
                   const OpMsgRequest& request,
                   AggregateCommandRequest aggregationRequest,
                   PrivilegeVector privileges)
            : CommandInvocation(cmd),
              _request(request),
              _dbName(request.getDatabase().toString()),
              _aggregationRequest(std::move(aggregationRequest)),
              _liteParsedPipeline(LiteParsedPipeline(_aggregationRequest)),
              _privileges(std::move(privileges)) {}

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return _liteParsedPipeline.supportsReadConcern(
                level,
                isImplicitDefault,
                _aggregationRequest.getExplain(),
                serverGlobalParams.enableMajorityReadConcern);
        }

        void _runAggCommand(OperationContext* opCtx,
                            const std::string& dbname,
                            const BSONObj& cmdObj,
                            BSONObjBuilder* result) {
            const auto& nss = _aggregationRequest.getNamespace();

            try {
                uassertStatusOK(
                    ClusterAggregate::runAggregate(opCtx,
                                                   ClusterAggregate::Namespaces{nss, nss},
                                                   _aggregationRequest,
                                                   _liteParsedPipeline,
                                                   _privileges,
                                                   result));
            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                // If the aggregation failed because the namespace is a view, re-run the command
                // with the resolved view pipeline and namespace.
                uassertStatusOK(ClusterAggregate::retryOnViewError(opCtx,
                                                                   _aggregationRequest,
                                                                   *ex.extraInfo<ResolvedView>(),
                                                                   nss,
                                                                   _privileges,
                                                                   result));
            }
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            CommandHelpers::handleMarkKillOnClientDisconnect(
                opCtx, !Pipeline::aggHasWriteStage(_request.body));

            Impl::checkCanRunHere(opCtx);

            auto bob = reply->getBodyBuilder();
            _runAggCommand(opCtx, _dbName, _request.body, &bob);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanExplainHere(opCtx);
            auto bodyBuilder = result->getBodyBuilder();
            _runAggCommand(opCtx, _dbName, _request.body, &bodyBuilder);
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            Impl::doCheckAuthorization(opCtx, _request, _privileges);
        }

        NamespaceString ns() const override {
            return _aggregationRequest.getNamespace();
        }

        const OpMsgRequest& _request;
        const std::string _dbName;
        AggregateCommandRequest _aggregationRequest;
        const LiteParsedPipeline _liteParsedPipeline;
        const PrivilegeVector _privileges;
    };

    std::string help() const override {
        return "Runs the sharded aggregation command. See "
               "http://dochub.mongodb.org/core/aggregation for more details.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    /**
     * A pipeline/aggregation command does not increment the command counter, but rather increments
     * the query counter.
     */
    bool shouldAffectCommandCounter() const override final {
        return false;
    }

    bool shouldAffectQueryCounter() const override final {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::AggregateCommandRequest::kAuthorizationContract;
    }

    bool allowedInTransactions() const final {
        return true;
    }
};

}  // namespace
}  // namespace mongo
