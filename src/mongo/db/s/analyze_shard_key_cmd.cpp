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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/analyze_shard_key_cmd_util.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_cmd_gen.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/analyze_shard_key_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(analyzeShardKeySkipCalcalutingKeyCharactericsMetrics);
MONGO_FAIL_POINT_DEFINE(analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics);

const std::string kOrphanDocsWarningMessage = "If \"" +
    KeyCharacteristicsMetrics::kNumOrphanDocsFieldName + "\" is large relative to \"" +
    KeyCharacteristicsMetrics::kNumDocsFieldName +
    "\", you may want to rerun the command at some other time to get more accurate \"" +
    KeyCharacteristicsMetrics::kNumDistinctValuesFieldName + "\" and \"" +
    KeyCharacteristicsMetrics::kMostCommonValuesFieldName + "\" metrics.";

class AnalyzeShardKeyCmd : public TypedCommand<AnalyzeShardKeyCmd> {
public:
    using Request = AnalyzeShardKey;
    using Response = AnalyzeShardKeyResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "analyzeShardKey command is not supported on a standalone mongod",
                    repl::ReplicationCoordinator::get(opCtx)->isReplEnabled());
            uassert(ErrorCodes::IllegalOperation,
                    "analyzeShardKey command is not supported on a configsvr mongod",
                    !serverGlobalParams.clusterRole.exclusivelyHasConfigRole());

            const auto& nss = ns();
            const auto& key = request().getKey();
            uassertStatusOK(validateNamespace(nss));
            const auto collUuid = uassertStatusOK(validateCollectionOptionsLocally(opCtx, nss));

            LOGV2(6875001, "Start analyzing shard key", logAttrs(nss), "shardKey"_attr = key);

            Response response;

            // Calculate metrics about the characteristics of the shard key.
            if (!MONGO_unlikely(
                    analyzeShardKeySkipCalcalutingKeyCharactericsMetrics.shouldFail())) {
                auto keyCharacteristics = analyze_shard_key::calculateKeyCharacteristicsMetrics(
                    opCtx, nss, collUuid, key);
                response.setKeyCharacteristics(keyCharacteristics);
                if (response.getNumOrphanDocs()) {
                    response.setNote(StringData(kOrphanDocsWarningMessage));
                }
            }

            // Calculate metrics about the read and write distribution from sampled queries. Query
            // sampling is not supported on multitenant replica sets.
            if (!gMultitenancySupport &&
                !MONGO_unlikely(
                    analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics.shouldFail())) {
                auto [readDistribution, writeDistribution] =
                    analyze_shard_key::calculateReadWriteDistributionMetrics(
                        opCtx, nss, collUuid, key);
                response.setReadDistribution(readDistribution);
                response.setWriteDistribution(writeDistribution);
            }

            return response;
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::shardCollection));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Returns metrics for evaluating a shard key for a collection.";
    }
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(AnalyzeShardKeyCmd,
                                       analyze_shard_key::gFeatureFlagAnalyzeShardKey);

}  // namespace

}  // namespace analyze_shard_key
}  // namespace mongo
