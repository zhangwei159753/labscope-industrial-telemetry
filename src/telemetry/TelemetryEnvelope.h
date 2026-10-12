#pragma once

#include <json/json.h>

#include <cstdint>
#include <string>

namespace labscope::telemetry
{

struct TelemetryEnvelope
{
    std::string siteId;
    std::string deviceId;
    int64_t seq{0};
    int64_t deviceTimeMs{0};
    int64_t receivedTimeMs{0};
    Json::Value metrics{Json::objectValue};
};

}  // namespace labscope::telemetry
