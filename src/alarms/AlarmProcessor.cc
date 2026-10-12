#include "AlarmProcessor.h"

#include <drogon/orm/Exception.h>
#include <json/json.h>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace labscope::alarms
{

struct AlarmProcessorState
{
    std::atomic<bool> running{false};
    std::atomic<bool> processing{false};
    std::mutex mutex;
    std::condition_variable condition;
};

namespace
{
using namespace drogon::orm;

struct CompletionInfo
{
    DbClientPtr dbClient;
    std::atomic<int64_t> eventId{0};
    std::atomic<bool> failed{false};
    std::mutex mutex;
    std::string error;
};

struct Work
{
    std::shared_ptr<AlarmProcessorState> processorState;
    DbClientPtr dbClient;
    std::shared_ptr<Transaction> transaction;
    int64_t eventId{0};
    telemetry::TelemetryEnvelope sample;
    std::vector<AlarmRule> rules;
    std::vector<AlarmEvaluationState> states;
    std::vector<int64_t> activeEventIds;
    std::size_t ruleIndex{0};
    bool failed{false};
    std::string error;
    std::shared_ptr<CompletionInfo> completion;
};

void fail(const std::shared_ptr<Work> &work, std::string message)
{
    if (work->failed)
        return;
    work->failed = true;
    work->error = std::move(message);
    if (work->completion)
    {
        work->completion->failed = true;
        if (work->eventId > 0)
            work->completion->eventId = work->eventId;
        std::lock_guard<std::mutex> lock(work->completion->mutex);
        work->completion->error = work->error;
    }
    if (work->processorState)
        work->processorState->processing = false;
    if (work->transaction)
    {
        work->transaction->rollback();
        work->transaction.reset();
    }
    if (work->eventId > 0)
    {
        work->dbClient->execSqlAsync(
            "UPDATE telemetry_outbox SET attempt_count = attempt_count + 1, last_error = $1 "
            "WHERE event_id = $2 AND processed_at IS NULL",
            [](const Result &) {}, [](const DrogonDbException &) {},
            work->error, work->eventId);
    }
}

void persistState(const std::shared_ptr<Work> &work,
                  std::size_t index,
                  int64_t activeEventId);

void processRules(const std::shared_ptr<Work> &work, std::size_t index)
{
    if (index >= work->rules.size())
    {
        work->transaction->execSqlAsync(
            "UPDATE telemetry_outbox SET processed_at = clock_timestamp(), last_error = NULL "
            "WHERE event_id = $1 AND processed_at IS NULL",
            [work](const Result &) { work->transaction.reset(); },
            [work](const DrogonDbException &error) { fail(work, error.base().what()); },
            work->eventId);
        return;
    }

    auto &rule = work->rules[index];
    AlarmEvaluator evaluator;
    evaluator.restore(rule, work->states[index]);
    const auto transitions = evaluator.onSample(work->sample, {rule});
    auto nextState = evaluator.state(rule);
    work->states[index] = nextState;
    if (transitions.empty())
    {
        persistState(work, index, work->activeEventIds[index]);
        return;
    }

    const auto &change = transitions.front();
    if (change.type == AlarmTransitionType::Triggered)
    {
        work->transaction->execSqlAsync(
            "INSERT INTO alarm_events "
            "(rule_id, site_id, device_id, metric_name, severity, state, trigger_seq, "
            "trigger_value, triggered_at) "
            "VALUES ($1, $2, $3, $4, $5, 'active', $6, $7, "
            "to_timestamp($8::double precision / 1000.0)) "
            "ON CONFLICT (rule_id, trigger_seq) DO UPDATE SET trigger_value = EXCLUDED.trigger_value "
            "RETURNING event_id",
            [work, index, nextState](const Result &rows) {
                if (rows.empty())
                {
                    fail(work, "alarm event insert returned no event id");
                    return;
                }
                work->activeEventIds[index] = rows[0]["event_id"].as<int64_t>();
                work->states[index] = nextState;
                persistState(work, index, work->activeEventIds[index]);
            },
            [work](const DrogonDbException &error) { fail(work, error.base().what()); },
            std::stoll(rule.id), rule.siteId, rule.deviceId, rule.metric,
            change.severity == AlarmSeverity::Critical ? "critical" :
                change.severity == AlarmSeverity::Info ? "info" : "warning",
            change.seq, change.value, change.timestampMs);
        return;
    }

    work->transaction->execSqlAsync(
        "UPDATE alarm_events SET state = 'recovered', recovered_at = "
        "to_timestamp($1::double precision / 1000.0) "
        "WHERE event_id = $2 AND state <> 'recovered'",
        [work, index, nextState](const Result &) {
            work->activeEventIds[index] = 0;
            work->states[index] = nextState;
            persistState(work, index, 0);
        },
        [work](const DrogonDbException &error) { fail(work, error.base().what()); },
        change.timestampMs, work->activeEventIds[index]);
}

void persistState(const std::shared_ptr<Work> &work,
                  std::size_t index,
                  int64_t activeEventId)
{
    auto &rule = work->rules[index];
    const auto &state = work->states[index];
    const int64_t thresholdSince = state.thresholdSinceMs.value_or(-1);
    work->transaction->execSqlAsync(
        "INSERT INTO alarm_rule_state "
        "(rule_id, last_seq, threshold_since_ms, cooldown_until_ms, active_event_id) "
        "VALUES ($1, $2, NULLIF($3::bigint, -1), $4, NULLIF($5::bigint, 0)) "
        "ON CONFLICT (rule_id) DO UPDATE SET last_seq = EXCLUDED.last_seq, "
        "threshold_since_ms = EXCLUDED.threshold_since_ms, "
        "cooldown_until_ms = EXCLUDED.cooldown_until_ms, "
        "active_event_id = EXCLUDED.active_event_id, updated_at = clock_timestamp()",
        [work, index](const Result &) { processRules(work, index + 1); },
        [work](const DrogonDbException &error) { fail(work, error.base().what()); },
        std::stoll(rule.id), state.lastSeq, thresholdSince,
        state.nextEligibleMs, activeEventId);
}

void onRulesLoaded(const std::shared_ptr<Work> &work, const Result &rows)
{
    const auto *debug = std::getenv("LABSCOPE_DEBUG_ALARMS");
    if (debug && std::string(debug) == "1")
    {
        std::cerr << "alarm_debug sample site=" << work->sample.siteId
                  << " device=" << work->sample.deviceId
                  << " seq=" << work->sample.seq
                  << " received_ms=" << work->sample.receivedTimeMs
                  << " rules=" << rows.size() << '\n';
    }
    work->rules.reserve(rows.size());
    work->states.reserve(rows.size());
    work->activeEventIds.reserve(rows.size());
    for (const auto &row : rows)
    {
        AlarmRule rule;
        rule.id = std::to_string(row["rule_id"].as<int64_t>());
        rule.siteId = row["site_id"].as<std::string>();
        rule.deviceId = row["device_id"].as<std::string>();
        rule.metric = row["metric_name"].as<std::string>();
        rule.direction = row["direction"].as<std::string>() == "below"
                             ? ThresholdDirection::Below
                             : ThresholdDirection::Above;
        rule.threshold = row["threshold"].as<double>();
        rule.durationMs = row["duration_ms"].as<int64_t>();
        rule.hysteresis = row["hysteresis"].as<double>();
        rule.cooldownMs = row["cooldown_ms"].as<int64_t>();
        const auto severity = row["severity"].as<std::string>();
        rule.severity = severity == "critical" ? AlarmSeverity::Critical
                       : severity == "info" ? AlarmSeverity::Info
                                              : AlarmSeverity::Warning;
        rule.enabled = row["enabled"].as<bool>();
        work->rules.push_back(std::move(rule));

        AlarmEvaluationState state;
        if (!row["last_seq"].isNull())
            state.lastSeq = row["last_seq"].as<int64_t>();
        if (!row["threshold_since_ms"].isNull())
            state.thresholdSinceMs = row["threshold_since_ms"].as<int64_t>();
        if (!row["cooldown_until_ms"].isNull())
            state.nextEligibleMs = row["cooldown_until_ms"].as<int64_t>();
        const int64_t activeEventId = row["active_event_id"].isNull()
                                          ? 0
                                          : row["active_event_id"].as<int64_t>();
        state.active = activeEventId > 0;
        work->states.push_back(state);
        work->activeEventIds.push_back(activeEventId);
    }
    processRules(work, 0);
}

void onOutboxRows(const std::shared_ptr<Work> &work, const Result &rows)
{
    if (rows.empty())
    {
        work->transaction.reset();
        return;
    }
    const auto &row = rows[0];
    work->eventId = row["event_id"].as<int64_t>();
    if (work->completion)
        work->completion->eventId = work->eventId;
    work->sample.siteId = row["site_id"].as<std::string>();
    work->sample.deviceId = row["device_id"].as<std::string>();
    work->sample.seq = row["seq"].as<int64_t>();
    work->sample.receivedTimeMs = row["received_time_ms"].as<int64_t>();
    Json::Value payload;
    Json::CharReaderBuilder builder;
    std::string parseError;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    const auto payloadText = row["payload"].as<std::string>();
    if (!reader->parse(payloadText.data(), payloadText.data() + payloadText.size(),
                       &payload, &parseError) || !payload.isObject() ||
        !payload["metrics"].isObject())
    {
        fail(work, "invalid telemetry outbox payload");
        return;
    }
    work->sample.metrics = payload["metrics"];

    work->transaction->execSqlAsync(
        "SELECT r.rule_id, r.site_id, r.device_id, r.metric_name, r.direction, "
        "r.threshold, r.duration_ms, r.hysteresis, r.cooldown_ms, r.severity, r.enabled, "
        "s.last_seq, s.threshold_since_ms, s.cooldown_until_ms, s.active_event_id "
        "FROM alarm_rules r LEFT JOIN alarm_rule_state s ON s.rule_id = r.rule_id "
        "WHERE r.site_id = $1 AND r.device_id = $2 "
        "AND r.created_at <= (SELECT received_at FROM telemetry_outbox WHERE event_id = $3::bigint) "
        "ORDER BY r.rule_id",
        [work](const Result &rules) { onRulesLoaded(work, rules); },
        [work](const DrogonDbException &error) { fail(work, error.base().what()); },
        work->sample.siteId, work->sample.deviceId, std::to_string(work->eventId));
}

void processPending(const std::shared_ptr<Work> &work)
{
    work->transaction->execSqlAsync(
        "SELECT event_id, site_id, device_id, seq, payload::text AS payload, "
        "(extract(epoch FROM received_at) * 1000)::bigint AS received_time_ms "
        "FROM telemetry_outbox ob WHERE ob.processed_at IS NULL "
        "AND ob.event_type = 'telemetry_ingested' "
        "AND NOT EXISTS (SELECT 1 FROM telemetry_outbox earlier "
        "WHERE earlier.site_id = ob.site_id AND earlier.device_id = ob.device_id "
        "AND earlier.processed_at IS NULL AND earlier.event_type = 'telemetry_ingested' "
        "AND earlier.event_id < ob.event_id) ORDER BY ob.event_id "
        "FOR UPDATE SKIP LOCKED LIMIT 1",
        [work](const Result &rows) { onOutboxRows(work, rows); },
        [work](const DrogonDbException &error) { fail(work, error.base().what()); });
}
}  // namespace

AlarmProcessor::AlarmProcessor(drogon::orm::DbClientPtr dbClient)
    : dbClient_(std::move(dbClient)),
      state_(std::make_shared<AlarmProcessorState>())
{
    if (!dbClient_)
        throw std::invalid_argument("AlarmProcessor requires a database client");
}

AlarmProcessor::~AlarmProcessor() { stop(); }

bool AlarmProcessor::isRunning() const noexcept
{
    return state_ && state_->running.load();
}

void AlarmProcessor::start()
{
    if (state_->running.exchange(true))
        return;
    thread_ = std::thread([this] { run(); });
}

void AlarmProcessor::stop()
{
    if (!state_->running.exchange(false))
        return;
    state_->condition.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void AlarmProcessor::run()
{
    std::unique_lock<std::mutex> lock(state_->mutex);
    while (state_->running)
    {
        state_->condition.wait_for(lock, std::chrono::milliseconds(250));
        if (!state_->running)
            break;
        if (state_->processing.exchange(true))
            continue;
        lock.unlock();

        auto work = std::make_shared<Work>();
        work->processorState = state_;
        work->dbClient = dbClient_;
        work->completion = std::make_shared<CompletionInfo>();
        work->completion->dbClient = dbClient_;
        auto completion = work->completion;
        dbClient_->newTransactionAsync(
            [work, completion, state = state_](const std::shared_ptr<drogon::orm::Transaction> &transaction) {
                if (!transaction)
                {
                    state->processing = false;
                    return;
                }
                work->transaction = transaction;
                transaction->setCommitCallback([completion, state](bool committed) {
                    if (!committed || completion->failed)
                    {
                        const auto eventId = completion->eventId.load();
                        if (eventId > 0)
                        {
                            std::string error = "alarm transaction failed";
                            {
                                std::lock_guard<std::mutex> lock(completion->mutex);
                                if (!completion->error.empty())
                                    error = completion->error;
                            }
                            completion->dbClient->execSqlAsync(
                                "UPDATE telemetry_outbox SET attempt_count = attempt_count + 1, "
                                "last_error = $1 WHERE event_id = $2 AND processed_at IS NULL",
                                [](const drogon::orm::Result &) {},
                                [](const drogon::orm::DrogonDbException &) {},
                                error, eventId);
                        }
                    }
                    state->processing = false;
                    if (committed && !completion->failed && completion->eventId > 0)
                        state->condition.notify_one();
                });
                processPending(work);
            },
            drogon::orm::TransactionType::Deferred);
        lock.lock();
    }
}

}  // namespace labscope::alarms
