#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace labscope::alarms
{

enum class ThresholdDirection { Above, Below };
enum class AlarmSeverity { Info, Warning, Critical };
enum class AlarmTransitionType { Triggered, Recovered };

struct AlarmRule
{
    std::string id;
    std::string siteId;
    std::string deviceId;
    std::string metric;
    ThresholdDirection direction{ThresholdDirection::Above};
    double threshold{0.0};
    int64_t durationMs{0};
    double hysteresis{0.0};
    int64_t cooldownMs{0};
    AlarmSeverity severity{AlarmSeverity::Warning};
    bool enabled{true};
};

struct AlarmTransition
{
    std::string ruleId;
    std::string siteId;
    std::string deviceId;
    std::string metric;
    AlarmSeverity severity{AlarmSeverity::Warning};
    AlarmTransitionType type{AlarmTransitionType::Triggered};
    int64_t seq{0};
    int64_t timestampMs{0};
    double value{0.0};
};

struct AlarmEvaluationState
{
    int64_t lastSeq{-1};
    std::optional<int64_t> thresholdSinceMs;
    int64_t nextEligibleMs{0};
    bool active{false};
};

}  // namespace labscope::alarms
