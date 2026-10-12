#include "AlarmEvaluator.h"

#include <cmath>
#include <limits>

namespace labscope::alarms
{
namespace
{
std::string stateKey(const AlarmRule &rule)
{
    return rule.siteId + "/" + rule.deviceId + "/" + rule.id;
}

bool valid(const AlarmRule &rule)
{
    return !rule.id.empty() && !rule.siteId.empty() && !rule.deviceId.empty() &&
           !rule.metric.empty() && std::isfinite(rule.threshold) &&
           std::isfinite(rule.hysteresis) && rule.hysteresis >= 0 &&
           rule.durationMs >= 0 && rule.cooldownMs >= 0;
}

bool crossed(const AlarmRule &rule, double value)
{
    return rule.direction == ThresholdDirection::Above
               ? value >= rule.threshold
               : value <= rule.threshold;
}

bool recovered(const AlarmRule &rule, double value)
{
    if (rule.direction == ThresholdDirection::Above)
    {
        const double boundary = rule.threshold - rule.hysteresis;
        return rule.hysteresis > 0 ? value <= boundary : value < rule.threshold;
    }
    const double boundary = rule.threshold + rule.hysteresis;
    return rule.hysteresis > 0 ? value >= boundary : value > rule.threshold;
}

AlarmTransition transition(const AlarmRule &rule,
                           const telemetry::TelemetryEnvelope &sample,
                           double value,
                           AlarmTransitionType type)
{
    return {rule.id, rule.siteId, rule.deviceId, rule.metric, rule.severity,
            type, sample.seq, sample.receivedTimeMs, value};
}
}  // namespace

void AlarmEvaluator::restore(const AlarmRule &rule, const AlarmEvaluationState &state)
{
    if (valid(rule))
        states_[stateKey(rule)] = {state.lastSeq, state.thresholdSinceMs,
                                   state.nextEligibleMs, state.active};
}

AlarmEvaluationState AlarmEvaluator::state(const AlarmRule &rule) const
{
    const auto it = states_.find(stateKey(rule));
    if (it == states_.end())
        return {};
    return {it->second.lastSeq, it->second.thresholdSinceMs,
            it->second.nextEligibleMs, it->second.active};
}

std::vector<AlarmTransition> AlarmEvaluator::onSample(
    const telemetry::TelemetryEnvelope &sample,
    const std::vector<AlarmRule> &rules)
{
    std::vector<AlarmTransition> transitions;
    if (sample.seq < 0 || sample.receivedTimeMs < 0 || !sample.metrics.isObject())
        return transitions;

    for (const auto &rule : rules)
    {
        if (!valid(rule) || rule.siteId != sample.siteId ||
            rule.deviceId != sample.deviceId)
            continue;
        auto &state = states_[stateKey(rule)];
        if (sample.seq <= state.lastSeq)
            continue;
        state.lastSeq = sample.seq;
        const auto &metric = sample.metrics[rule.metric];
        if (!metric.isNumeric() || !std::isfinite(metric.asDouble()))
        {
            if (!state.active)
                state.thresholdSinceMs.reset();
            continue;
        }
        const double value = metric.asDouble();
        if (!rule.enabled)
        {
            state.thresholdSinceMs.reset();
            if (state.active && recovered(rule, value))
            {
                state.active = false;
                transitions.push_back(transition(rule, sample, value,
                                                  AlarmTransitionType::Recovered));
            }
            continue;
        }
        if (state.active)
        {
            if (recovered(rule, value))
            {
                state.active = false;
                state.thresholdSinceMs.reset();
                transitions.push_back(transition(rule, sample, value,
                                                  AlarmTransitionType::Recovered));
            }
            continue;
        }

        if (!crossed(rule, value))
        {
            state.thresholdSinceMs.reset();
            continue;
        }
        if (!state.thresholdSinceMs)
            state.thresholdSinceMs = sample.receivedTimeMs;

        const auto elapsed = sample.receivedTimeMs >= *state.thresholdSinceMs
                                 ? sample.receivedTimeMs - *state.thresholdSinceMs
                                 : 0;
        if (elapsed < rule.durationMs || sample.receivedTimeMs < state.nextEligibleMs)
            continue;

        state.active = true;
        state.thresholdSinceMs.reset();
        state.nextEligibleMs = rule.cooldownMs >
                                       std::numeric_limits<int64_t>::max() - sample.receivedTimeMs
                                   ? std::numeric_limits<int64_t>::max()
                                   : sample.receivedTimeMs + rule.cooldownMs;
        transitions.push_back(transition(rule, sample, value,
                                          AlarmTransitionType::Triggered));
    }
    return transitions;
}

}  // namespace labscope::alarms
