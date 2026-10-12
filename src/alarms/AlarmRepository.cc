#include "AlarmRepository.h"

#include <drogon/orm/Exception.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace labscope::alarms
{
namespace
{
struct AckOutcome
{
    AcknowledgeResult result;
    AlarmRepository::AcknowledgeCallback callback;
    std::atomic<bool> delivered{false};
};

struct RuleOutcome
{
    CreateRuleResult result;
    AlarmRepository::CreateRuleCallback callback;
    std::atomic<bool> delivered{false};
};

void deliver(const std::shared_ptr<AckOutcome> &outcome, bool committed)
{
    if (outcome->delivered.exchange(true)) return;
    if (committed && outcome->result.error.empty() && outcome->result.found)
        outcome->result.succeeded = true;
    if (!committed && outcome->result.error.empty())
        outcome->result.error = "alarm acknowledgment transaction did not commit";
    if (outcome->callback)
    {
        auto callback = std::move(outcome->callback);
        callback(std::move(outcome->result));
    }
}

void deliver(const std::shared_ptr<RuleOutcome> &outcome, bool committed)
{
    if (outcome->delivered.exchange(true)) return;
    if (committed && outcome->result.error.empty() && outcome->result.ruleId > 0)
        outcome->result.succeeded = true;
    if (!committed && outcome->result.error.empty())
        outcome->result.error = "alarm rule transaction did not commit";
    if (outcome->callback)
    {
        auto callback = std::move(outcome->callback);
        callback(std::move(outcome->result));
    }
}
}  // namespace

AlarmRepository::AlarmRepository(drogon::orm::DbClientPtr dbClient)
    : dbClient_(std::move(dbClient))
{
    if (!dbClient_)
        throw std::invalid_argument("AlarmRepository requires a database client");
}

void AlarmRepository::createRule(const CreateRule &rule,
                                 int64_t userId,
                                 CreateRuleCallback callback) const
{
    auto outcome = std::make_shared<RuleOutcome>();
    outcome->callback = std::move(callback);
    dbClient_->newTransactionAsync(
        [outcome, rule, userId](
            const std::shared_ptr<drogon::orm::Transaction> &transaction) {
            if (!transaction)
            {
                outcome->result.error = "database transaction unavailable";
                deliver(outcome, false);
                return;
            }
            transaction->setCommitCallback([outcome](bool committed) { deliver(outcome, committed); });
            transaction->execSqlAsync(
                "INSERT INTO alarm_rules "
                "(site_id, device_id, metric_name, direction, threshold, duration_ms, "
                "hysteresis, cooldown_ms, severity, created_by) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) RETURNING rule_id",
                [transaction, outcome, rule, userId](const drogon::orm::Result &rows) {
                    if (rows.empty())
                    {
                        outcome->result.error = "alarm rule insert returned no id";
                        deliver(outcome, false);
                        transaction->rollback();
                        return;
                    }
                    outcome->result.ruleId = rows[0]["rule_id"].as<int64_t>();
                    transaction->execSqlAsync(
                        "INSERT INTO audit_log "
                        "(user_id, action, entity_type, entity_id, site_id, metadata) "
                        "VALUES ($1, 'alarm_rule.create', 'alarm_rule', $2, $3, "
                        "jsonb_build_object('device_id', $4::text, 'metric', $5::text, "
                        "'direction', $6::text, 'threshold', $7::double precision, "
                        "'duration_ms', $8::bigint, 'hysteresis', $9::double precision, "
                        "'cooldown_ms', $10::bigint, 'severity', $11::text))",
                        [transaction](const drogon::orm::Result &) {},
                        [transaction, outcome](const drogon::orm::DrogonDbException &error) {
                            outcome->result.error = error.base().what();
                            deliver(outcome, false);
                            transaction->rollback();
                        },
                        userId, std::to_string(outcome->result.ruleId), rule.siteId,
                        rule.deviceId, rule.metric,
                        rule.direction == ThresholdDirection::Above ? "above" : "below",
                        rule.threshold, rule.durationMs, rule.hysteresis,
                        rule.cooldownMs,
                        rule.severity == AlarmSeverity::Critical ? "critical" :
                            rule.severity == AlarmSeverity::Info ? "info" : "warning");
                },
                [transaction, outcome](const drogon::orm::DrogonDbException &error) {
                    outcome->result.error = error.base().what();
                    deliver(outcome, false);
                    transaction->rollback();
                },
                rule.siteId, rule.deviceId, rule.metric,
                rule.direction == ThresholdDirection::Above ? "above" : "below",
                rule.threshold, rule.durationMs, rule.hysteresis, rule.cooldownMs,
                rule.severity == AlarmSeverity::Critical ? "critical" :
                    rule.severity == AlarmSeverity::Info ? "info" : "warning",
                userId);
        },
        drogon::orm::TransactionType::Deferred);
}

void AlarmRepository::acknowledge(int64_t eventId,
                                  const std::string &siteId,
                                  int64_t userId,
                                  const std::string &remark,
                                  AcknowledgeCallback callback) const
{
    auto outcome = std::make_shared<AckOutcome>();
    outcome->callback = std::move(callback);
    dbClient_->newTransactionAsync(
        [outcome, eventId, siteId, userId, remark](
            const std::shared_ptr<drogon::orm::Transaction> &transaction) {
            if (!transaction)
            {
                outcome->result.error = "database transaction unavailable";
                deliver(outcome, false);
                return;
            }
            transaction->setCommitCallback([outcome](bool committed) {
                deliver(outcome, committed);
            });
            transaction->execSqlAsync(
                "UPDATE alarm_events SET state = 'acknowledged', acknowledged_by = $1, "
                "acknowledged_at = clock_timestamp(), acknowledgment_remark = $2 "
                "WHERE event_id = $3 AND site_id = $4 AND state = 'active' "
                "RETURNING event_id, rule_id, device_id",
                [transaction, outcome, userId, remark, siteId](
                    const drogon::orm::Result &rows) {
                    if (rows.empty())
                    {
                        outcome->result.found = false;
                        outcome->result.error = "event not found or is no longer active";
                        deliver(outcome, false);
                        transaction->rollback();
                        return;
                    }
                    outcome->result.found = true;
                    const auto eventId = rows[0]["event_id"].as<int64_t>();
                    const auto ruleId = rows[0]["rule_id"].as<int64_t>();
                    const auto deviceId = rows[0]["device_id"].as<std::string>();
                    transaction->execSqlAsync(
                        "INSERT INTO audit_log "
                        "(user_id, action, entity_type, entity_id, site_id, metadata) "
                        "VALUES ($1, 'alarm.acknowledge', 'alarm_event', $2, $3, "
                        "jsonb_build_object('rule_id', $4::bigint, 'device_id', $5::text, "
                        "'remark', $6::text))",
                        [transaction](const drogon::orm::Result &) {},
                        [transaction, outcome](const drogon::orm::DrogonDbException &error) {
                            outcome->result.error = error.base().what();
                            deliver(outcome, false);
                            transaction->rollback();
                        },
                        userId, std::to_string(eventId), siteId, ruleId, deviceId, remark);
                },
                [transaction, outcome](const drogon::orm::DrogonDbException &error) {
                    outcome->result.error = error.base().what();
                    deliver(outcome, false);
                    transaction->rollback();
                },
                userId, remark, eventId, siteId);
        },
        drogon::orm::TransactionType::Deferred);
}

}  // namespace labscope::alarms
