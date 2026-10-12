#include "telemetry_hub.h"

#include <chrono>
#include <cmath>
#include <cctype>
#include <utility>
#include <vector>

namespace
{
constexpr size_t kMaxDevices = 1024;
constexpr size_t kHistoryPerDevice = 256;

int64_t currentTimeMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

TelemetryHub &telemetryHub()
{
    static TelemetryHub hub;
    return hub;
}

TelemetryHub::~TelemetryHub()
{
    running_ = false;
    if (dispatcher_.joinable())
        dispatcher_.join();
}

bool TelemetryHub::validDeviceId(const std::string &id)
{
    if (id.empty() || id.size() > 64)
        return false;
    for (unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    return true;
}

IngestResult TelemetryHub::reject(const std::string &reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++rejected_;
    return {false, reason};
}

IngestResult TelemetryHub::ingest(const std::string &deviceId,
                                  const Json::Value &sample)
{
    if (!validDeviceId(deviceId))
        return reject("invalid device_id");
    if (!sample.isObject() || !sample["seq"].isInt64() ||
        sample["seq"].asInt64() < 0)
        return reject("seq must be a non-negative integer");
    if (!sample["timestamp_ms"].isInt64() ||
        sample["timestamp_ms"].asInt64() < 0)
        return reject("timestamp_ms must be a non-negative integer");
    const auto &metrics = sample["metrics"];
    if (!metrics.isObject() || metrics.empty() || metrics.size() > 32)
        return reject("metrics must contain 1-32 numeric values");
    for (const auto &name : metrics.getMemberNames())
    {
        if (name.empty() || name.size() > 64 || !metrics[name].isNumeric() ||
            !std::isfinite(metrics[name].asDouble()))
            return reject("metrics must contain finite named numbers");
    }

    const auto seq = sample["seq"].asInt64();
    Json::Value event;
    event["type"] = "telemetry";
    event["device_id"] = deviceId;
    event["seq"] = Json::Int64(seq);
    event["timestamp_ms"] = sample["timestamp_ms"];
    event["received_ms"] = Json::Int64(currentTimeMs());
    event["metrics"] = metrics;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    auto wire = std::make_shared<const std::string>(Json::writeString(writer, event));

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(deviceId);
    if (it == devices_.end())
    {
        if (devices_.size() >= kMaxDevices)
        {
            ++rejected_;
            return {false, "device capacity reached"};
        }
        it = devices_.emplace(deviceId, DeviceState{}).first;
    }
    auto &device = it->second;
    if (seq <= device.lastSeq)
    {
        ++rejected_;
        return {false, "seq must increase per device"};
    }
    device.lastSeq = seq;
    device.history.push_back(event);
    if (device.history.size() > kHistoryPerDevice)
        device.history.pop_front();
    device.latestWire = std::move(wire);
    ++accepted_;
    return {true, ""};
}

void TelemetryHub::publishCommitted(const std::string &siteId,
                                   const std::string &deviceId,
                                   int64_t seq,
                                   int64_t deviceTimeMs,
                                   int64_t receivedTimeMs,
                                   const Json::Value &metrics)
{
    if (!validDeviceId(siteId) || !validDeviceId(deviceId) || seq < 0)
        return;

    const std::string key = siteId + "/" + deviceId;
    Json::Value event(Json::objectValue);
    event["type"] = "telemetry";
    event["site_id"] = siteId;
    event["device_id"] = deviceId;
    event["seq"] = Json::Int64(seq);
    event["timestamp_ms"] = Json::Int64(deviceTimeMs);
    event["received_ms"] = Json::Int64(receivedTimeMs);
    event["metrics"] = metrics;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    auto wire = std::make_shared<const std::string>(Json::writeString(writer, event));

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(key);
    if (it == devices_.end())
    {
        if (devices_.size() >= kMaxDevices)
            return;
        it = devices_.emplace(key, DeviceState{}).first;
    }
    auto &device = it->second;
    if (seq < device.lastSeq)
        return;
    event["received_ms"] = Json::Int64(receivedTimeMs);
    if (seq == device.lastSeq && !device.history.empty())
        device.history.back() = event;
    else
    {
        device.lastSeq = seq;
        device.history.push_back(event);
        if (device.history.size() > kHistoryPerDevice)
            device.history.pop_front();
        ++accepted_;
    }
    device.latestWire = std::move(wire);
}

std::optional<Json::Value> TelemetryHub::latest(const std::string &deviceId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(deviceId);
    if (it == devices_.end() || it->second.history.empty())
        return std::nullopt;
    return it->second.history.back();
}

std::optional<Json::Value> TelemetryHub::history(const std::string &deviceId,
                                                  int64_t afterSeq,
                                                  size_t limit) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(deviceId);
    if (it == devices_.end())
        return std::nullopt;

    Json::Value result;
    result["device_id"] = deviceId;
    result["samples"] = Json::arrayValue;
    result["next_after_seq"] = Json::Int64(afterSeq);
    for (const auto &sample : it->second.history)
    {
        const auto seq = sample["seq"].asInt64();
        if (seq <= afterSeq)
            continue;
        result["samples"].append(sample);
        result["next_after_seq"] = Json::Int64(seq);
        if (result["samples"].size() == limit)
            break;
    }
    return result;
}

Json::Value TelemetryHub::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value value;
    value["accepted"] = Json::UInt64(accepted_);
    value["rejected"] = Json::UInt64(rejected_);
    value["devices"] = Json::UInt64(devices_.size());
    value["monitors"] = Json::UInt64(monitors_.size());
    value["history_per_device"] = Json::UInt64(kHistoryPerDevice);
    value["monitor_push_interval_ms"] = 50;
    return value;
}

uint64_t TelemetryHub::addMonitor(
    const std::string &deviceId,
    const drogon::WebSocketConnectionPtr &connection)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = nextMonitorId_++;
    monitors_.emplace(id, MonitorState{deviceId, connection, -1});
    return id;
}

void TelemetryHub::removeMonitor(uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    monitors_.erase(id);
}

void TelemetryHub::start()
{
    if (!running_.exchange(true))
        dispatcher_ = std::thread([this] { dispatchLoop(); });
}

void TelemetryHub::dispatchLoop()
{
    while (running_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        dispatchLatest();
    }
}

void TelemetryHub::dispatchLatest()
{
    using Delivery = std::pair<drogon::WebSocketConnectionPtr,
                               std::shared_ptr<const std::string>>;
    std::vector<Delivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = monitors_.begin(); it != monitors_.end();)
        {
            auto connection = it->second.connection.lock();
            if (!connection || !connection->connected())
            {
                it = monitors_.erase(it);
                continue;
            }
            auto device = devices_.find(it->second.deviceId);
            if (device != devices_.end() && device->second.latestWire &&
                device->second.lastSeq != it->second.lastSentSeq)
            {
                deliveries.emplace_back(connection, device->second.latestWire);
                it->second.lastSentSeq = device->second.lastSeq;
            }
            ++it;
        }
    }
    for (const auto &delivery : deliveries)
        if (delivery.first->connected())
            delivery.first->send(*delivery.second);
}
