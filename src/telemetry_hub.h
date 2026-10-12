#pragma once

#include <drogon/WebSocketConnection.h>
#include <json/json.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

struct IngestResult
{
    bool accepted{false};
    std::string reason;
};

class TelemetryHub
{
  public:
    ~TelemetryHub();

    static bool validDeviceId(const std::string &deviceId);
    IngestResult ingest(const std::string &deviceId, const Json::Value &sample);
    IngestResult reject(const std::string &reason);
    void publishCommitted(const std::string &siteId,
                         const std::string &deviceId,
                         int64_t seq,
                         int64_t deviceTimeMs,
                         int64_t receivedTimeMs,
                         const Json::Value &metrics);
    std::optional<Json::Value> latest(const std::string &deviceId) const;
    std::optional<Json::Value> history(const std::string &deviceId,
                                       int64_t afterSeq,
                                       size_t limit) const;
    Json::Value stats() const;
    uint64_t addMonitor(const std::string &deviceId,
                        const drogon::WebSocketConnectionPtr &connection);
    void removeMonitor(uint64_t id);
    void start();

  private:
    struct DeviceState
    {
        int64_t lastSeq{-1};
        std::deque<Json::Value> history;
        std::shared_ptr<const std::string> latestWire;
    };
    struct MonitorState
    {
        std::string deviceId;
        std::weak_ptr<drogon::WebSocketConnection> connection;
        int64_t lastSentSeq{-1};
    };

    void dispatchLoop();
    void dispatchLatest();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, DeviceState> devices_;
    std::unordered_map<uint64_t, MonitorState> monitors_;
    uint64_t nextMonitorId_{1};
    uint64_t accepted_{0};
    uint64_t rejected_{0};
    std::atomic<bool> running_{false};
    std::thread dispatcher_;
};

TelemetryHub &telemetryHub();
