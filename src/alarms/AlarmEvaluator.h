#pragma once

#include "AlarmRule.h"
#include "../telemetry/TelemetryEnvelope.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace labscope::alarms
{

class AlarmEvaluator final
{
  public:
    void restore(const AlarmRule &rule, const AlarmEvaluationState &state);
    AlarmEvaluationState state(const AlarmRule &rule) const;
    std::vector<AlarmTransition> onSample(
        const telemetry::TelemetryEnvelope &sample,
        const std::vector<AlarmRule> &rules);

  private:
    struct State
    {
        int64_t lastSeq{-1};
        std::optional<int64_t> thresholdSinceMs;
        int64_t nextEligibleMs{0};
        bool active{false};
    };

    std::unordered_map<std::string, State> states_;
};

}  // namespace labscope::alarms
