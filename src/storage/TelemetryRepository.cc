#include "TelemetryRepository.h"

#include <drogon/orm/Exception.h>
#include <json/json.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace labscope::storage
{
namespace
{

constexpr char kPersistSql[] = R"SQL(
WITH input AS MATERIALIZED (
    SELECT $1::text AS site_id,
           $2::text AS device_id,
           $3::bigint AS seq,
           $4::double precision AS device_time_ms,
           $5::double precision AS received_time_ms,
           $6::jsonb AS metrics,
           $7::jsonb AS event_payload
),
device AS MATERIALIZED (
    SELECT d.device_pk, d.site_id, d.device_id, d.enabled, d.last_seq
    FROM devices d, input i
    WHERE d.site_id = i.site_id AND d.device_id = i.device_id
    FOR UPDATE OF d
),
advanced AS (
    UPDATE devices d
       SET last_seq = i.seq,
           next_source_seq = GREATEST(d.next_source_seq, i.seq + 1),
           sequence_gap_count = d.sequence_gap_count +
               GREATEST(i.seq - locked.last_seq - 1, 0),
           last_seen_at = to_timestamp(i.received_time_ms / 1000.0),
           last_telemetry_at = to_timestamp(i.received_time_ms / 1000.0),
           last_telemetry = jsonb_build_object(
               'seq', i.seq,
               'device_time_ms', i.device_time_ms,
               'received_time_ms', i.received_time_ms,
               'metrics', i.metrics),
           updated_at = clock_timestamp()
      FROM device locked, input i
     WHERE d.device_pk = locked.device_pk
       AND locked.enabled
       AND i.seq > locked.last_seq
    RETURNING d.device_pk, d.site_id, d.device_id
),
raw_insert AS (
    INSERT INTO telemetry_samples
        (site_id, device_id, received_at, seq, device_time, metrics, schema_version)
    SELECT a.site_id, a.device_id,
           to_timestamp(i.received_time_ms / 1000.0), i.seq,
           to_timestamp(i.device_time_ms / 1000.0), i.metrics, 1
      FROM advanced a CROSS JOIN input i
    RETURNING 1
),
aggregate_insert AS (
    INSERT INTO telemetry_1m
        (site_id, device_id, bucket_start, metric_name,
         min_value, max_value, sum_value, sample_count, updated_at)
    SELECT a.site_id, a.device_id,
           date_trunc('minute', to_timestamp(i.received_time_ms / 1000.0)),
           metric.key, metric.value::double precision, metric.value::double precision,
           metric.value::double precision, 1, clock_timestamp()
      FROM advanced a CROSS JOIN input i
      CROSS JOIN LATERAL jsonb_each_text(i.metrics) AS metric(key, value)
    ON CONFLICT (site_id, device_id, bucket_start, metric_name)
    DO UPDATE SET min_value = LEAST(telemetry_1m.min_value, EXCLUDED.min_value),
                  max_value = GREATEST(telemetry_1m.max_value, EXCLUDED.max_value),
                  sum_value = telemetry_1m.sum_value + EXCLUDED.sum_value,
                  sample_count = telemetry_1m.sample_count + 1,
                  updated_at = clock_timestamp()
    RETURNING 1
),
outbox_insert AS (
    INSERT INTO telemetry_outbox
        (site_id, device_id, received_at, seq, event_type, payload)
    SELECT a.site_id, a.device_id,
           to_timestamp(i.received_time_ms / 1000.0), i.seq,
           'telemetry_ingested', i.event_payload
      FROM advanced a CROSS JOIN input i
    ON CONFLICT (site_id, device_id, received_at, seq, event_type) DO NOTHING
    RETURNING 1
)
SELECT CASE
    WHEN NOT EXISTS (SELECT 1 FROM device) THEN 'rejected'
    WHEN NOT EXISTS (SELECT 1 FROM device WHERE enabled) THEN 'rejected'
    WHEN EXISTS (SELECT 1 FROM advanced) THEN 'persisted'
    ELSE 'duplicate'
END AS decision
)SQL";

struct BatchState
{
    std::vector<ingest::QueuedTelemetry> batch;
    std::vector<ingest::IngestionResult> results;
    TelemetryRepository::BatchCompletionCallback completion;
    std::atomic<bool> completed{false};
};

void finish(const std::shared_ptr<BatchState> &state,
            std::vector<ingest::IngestionResult> results)
{
    if (state->completed.exchange(true))
        return;
    if (state->completion)
    {
        auto callback = std::move(state->completion);
        callback(std::move(results));
    }
}

std::vector<ingest::IngestionResult> retryableResults(std::size_t count,
                                                       const std::string &reason)
{
    return std::vector<ingest::IngestionResult>(
        count, {ingest::IngestionStatus::RetryableError, reason});
}

void executeNext(const std::shared_ptr<BatchState> &state,
                 const std::shared_ptr<drogon::orm::Transaction> &transaction,
                 std::size_t index)
{
    if (index == state->batch.size())
        return;  // Transaction destruction commits; the commit callback reports results.

    const auto &envelope = state->batch[index].envelope;
    Json::Value event(Json::objectValue);
    event["schema_version"] = 1;
    event["seq"] = Json::Int64(envelope.seq);
    event["device_time_ms"] = Json::Int64(envelope.deviceTimeMs);
    event["received_time_ms"] = Json::Int64(envelope.receivedTimeMs);
    event["metrics"] = envelope.metrics;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";

    transaction->execSqlAsync(
        kPersistSql,
        [state, transaction, index](const drogon::orm::Result &result) {
            const auto decision = result[0]["decision"].as<std::string>();
            if (decision == "persisted")
                state->results[index] = {ingest::IngestionStatus::Persisted, {}};
            else if (decision == "duplicate")
                state->results[index] = {ingest::IngestionStatus::Duplicate, {}};
            else
                state->results[index] = {ingest::IngestionStatus::Rejected,
                                         "device is missing or disabled"};
            executeNext(state, transaction, index + 1);
        },
        [state](const drogon::orm::DrogonDbException &error) {
            finish(state, retryableResults(state->batch.size(), error.base().what()));
        },
        envelope.siteId,
        envelope.deviceId,
        envelope.seq,
        static_cast<double>(envelope.deviceTimeMs),
        static_cast<double>(envelope.receivedTimeMs),
        envelope.metrics.toStyledString(),
        Json::writeString(writer, event));
}

}  // namespace

TelemetryRepository::TelemetryRepository(DbClientPtr dbClient)
    : dbClient_(std::move(dbClient))
{
    if (!dbClient_)
        throw std::invalid_argument("TelemetryRepository requires a DbClient");
}

void TelemetryRepository::insertBatch(std::vector<ingest::QueuedTelemetry> batch,
                                      BatchCompletionCallback completion) const
{
    if (batch.empty())
    {
        if (completion)
            completion({});
        return;
    }

    auto state = std::make_shared<BatchState>();
    state->batch = std::move(batch);
    state->results.resize(state->batch.size());
    state->completion = std::move(completion);
    const auto count = state->batch.size();

    dbClient_->newTransactionAsync(
        [state, count](const std::shared_ptr<drogon::orm::Transaction> &transaction) {
            if (!transaction)
            {
                finish(state, retryableResults(count, "database transaction unavailable"));
                return;
            }
            transaction->setCommitCallback([state, count](bool committed) {
                if (committed)
                    finish(state, std::move(state->results));
                else
                    finish(state, retryableResults(count, "database transaction commit failed"));
            });
            executeNext(state, transaction, 0);
        },
        drogon::orm::TransactionType::Deferred);
}

}  // namespace labscope::storage
