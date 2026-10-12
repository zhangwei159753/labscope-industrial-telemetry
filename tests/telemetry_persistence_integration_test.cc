#include "storage/DeviceRepository.h"
#include "storage/TelemetryRepository.h"

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr char kSiteId[] = "labscope-persistence-test";
constexpr char kDeviceId[] = "transaction-device";

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

labscope::ingest::QueuedTelemetry sample(int64_t seq, int value)
{
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    labscope::telemetry::TelemetryEnvelope envelope;
    envelope.siteId = kSiteId;
    envelope.deviceId = kDeviceId;
    envelope.seq = seq;
    envelope.deviceTimeMs = nowMs + seq * 1000;
    envelope.receivedTimeMs = nowMs + seq * 1000;
    envelope.metrics["temperature_c"] = value;
    return {std::move(envelope), {}};
}

std::vector<labscope::ingest::IngestionResult> persist(
    const labscope::storage::TelemetryRepository &repository,
    std::vector<labscope::ingest::QueuedTelemetry> batch)
{
    auto promise = std::make_shared<
        std::promise<std::vector<labscope::ingest::IngestionResult>>>();
    auto future = promise->get_future();
    repository.insertBatch(std::move(batch), [promise](auto results) {
        promise->set_value(std::move(results));
    });
    require(future.wait_for(std::chrono::seconds(15)) == std::future_status::ready,
            "telemetry persistence callback timed out");
    return future.get();
}

}  // namespace

int main()
{
    try
    {
        const char *connectionInfo = std::getenv("LABSCOPE_TEST_DB_CONNINFO");
        require(connectionInfo && *connectionInfo,
                "LABSCOPE_TEST_DB_CONNINFO must point at a migrated test database");
        auto db = drogon::orm::DbClient::newPgClient(connectionInfo, 4, true);
        db->execSqlSync("SELECT labscope_ensure_telemetry_partitions()");
        db->execSqlSync(
            "INSERT INTO sites (site_id, display_name) VALUES (?, 'Persistence Test') "
            "ON CONFLICT (site_id) DO NOTHING",
            kSiteId);
        db->execSqlSync(
            "INSERT INTO devices (site_id, device_id, display_name, mqtt_username, "
            "credential_hash, enabled) VALUES (?, ?, 'Transaction Device', ?, "
            "decode('00', 'hex'), true) ON CONFLICT (site_id, device_id) DO UPDATE "
            "SET enabled = true, last_seq = -1, sequence_gap_count = 0, "
            "next_source_seq = 0",
            kSiteId,
            kDeviceId,
            std::string(kSiteId) + "-" + kDeviceId);
        db->execSqlSync("DELETE FROM telemetry_outbox WHERE site_id = ?", kSiteId);
        db->execSqlSync("DELETE FROM telemetry_samples WHERE site_id = ?", kSiteId);
        db->execSqlSync("DELETE FROM telemetry_1m WHERE site_id = ?", kSiteId);

        labscope::storage::TelemetryRepository repository(db);
        auto committed = persist(repository, {sample(0, 20), sample(2, 24)});
        require(committed.size() == 2, "batch must return one result per input sample");
        require(committed[0].status == labscope::ingest::IngestionStatus::Persisted &&
                    committed[1].status == labscope::ingest::IngestionStatus::Persisted,
                "new sequences must persist");

        auto replayed = persist(repository, {sample(2, 24)});
        require(replayed.size() == 1 &&
                    replayed[0].status == labscope::ingest::IngestionStatus::Duplicate,
                "replay after commit must be acknowledged as duplicate");
        auto stale = persist(repository, {sample(1, 21)});
        require(stale.size() == 1 &&
                    stale[0].status == labscope::ingest::IngestionStatus::Duplicate,
                "stale sequence must not be inserted");

        auto atomicBatch = std::vector<labscope::ingest::QueuedTelemetry>{
            sample(3, 25), sample(4, 26)};
        atomicBatch[1].envelope.metrics["broken_metric"] = "not-a-number";
        auto rolledBack = persist(repository, std::move(atomicBatch));
        require(rolledBack.size() == 2 &&
                    rolledBack[0].status == labscope::ingest::IngestionStatus::RetryableError &&
                    rolledBack[1].status == labscope::ingest::IngestionStatus::RetryableError,
                "a SQL failure must mark the entire atomic batch retryable");

        auto device = db->execSqlSync(
            "SELECT last_seq, sequence_gap_count, next_source_seq FROM devices "
            "WHERE site_id = ? AND device_id = ?",
            kSiteId,
            kDeviceId);
        require(device.size() == 1 && device[0]["last_seq"].as<int64_t>() == 2 &&
                    device[0]["sequence_gap_count"].as<int64_t>() == 1 &&
                    device[0]["next_source_seq"].as<int64_t>() == 3,
                "watermark, sequence gap and source allocator must be consistent");
        auto counts = db->execSqlSync(
            "SELECT (SELECT count(*) FROM telemetry_samples WHERE site_id = ?) AS samples, "
            "       (SELECT count(*) FROM telemetry_outbox WHERE site_id = ?) AS outbox",
            kSiteId,
            kSiteId);
        require(counts[0]["samples"].as<int64_t>() == 2 &&
                    counts[0]["outbox"].as<int64_t>() == 2,
                "replay and rolled-back rows must not create duplicate samples or outbox events");

        labscope::storage::DeviceRepository devices(db);
        auto reservation = std::make_shared<std::promise<std::optional<int64_t>>>();
        auto sequenceFuture = reservation->get_future();
        devices.reserveSourceSequence(kSiteId, kDeviceId,
                                      [reservation](auto sequence, std::string error) {
            if (!error.empty())
                reservation->set_exception(
                    std::make_exception_ptr(std::runtime_error(std::move(error))));
            else
                reservation->set_value(sequence);
        });
        require(sequenceFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
                "source sequence reservation timed out");
        auto sequence = sequenceFuture.get();
        require(sequence && *sequence == 3,
                "reserved source sequence must follow the committed telemetry watermark");

        std::cout << "PostgreSQL persistence integration scenarios passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << "persistence integration failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
