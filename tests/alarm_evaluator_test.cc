#include "alarms/AlarmEvaluator.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

labscope::telemetry::TelemetryEnvelope sample(int64_t seq, int64_t receivedMs,
                                               double value)
{
    labscope::telemetry::TelemetryEnvelope envelope;
    envelope.siteId = "plant-a";
    envelope.deviceId = "pump-1";
    envelope.seq = seq;
    envelope.receivedTimeMs = receivedMs;
    envelope.metrics["temperature_c"] = value;
    return envelope;
}

labscope::alarms::AlarmRule aboveRule()
{
    labscope::alarms::AlarmRule rule;
    rule.id = "rule-1";
    rule.siteId = "plant-a";
    rule.deviceId = "pump-1";
    rule.metric = "temperature_c";
    rule.direction = labscope::alarms::ThresholdDirection::Above;
    rule.threshold = 10.0;
    rule.severity = labscope::alarms::AlarmSeverity::Warning;
    return rule;
}
}

int main()
{
    try
    {
        using namespace labscope::alarms;
        {
            AlarmEvaluator evaluator;
            auto rule = aboveRule();
            auto result = evaluator.onSample(sample(1, 1000, 9.9), {rule});
            require(result.empty(), "below threshold must not trigger an above rule");
            result = evaluator.onSample(sample(2, 2000, 10.0), {rule});
            require(result.size() == 1 && result[0].type == AlarmTransitionType::Triggered,
                    "equality satisfies an above threshold");
        }
        {
            AlarmEvaluator evaluator;
            auto rule = aboveRule();
            rule.durationMs = 1000;
            require(evaluator.onSample(sample(1, 1000, 11), {rule}).empty(),
                    "duration must not trigger on first crossing");
            require(evaluator.onSample(sample(2, 1999, 11), {rule}).empty(),
                    "duration must wait for full interval");
            auto result = evaluator.onSample(sample(3, 2000, 11), {rule});
            require(result.size() == 1 && result[0].type == AlarmTransitionType::Triggered,
                    "duration crossing should trigger at the boundary");
        }
        {
            AlarmEvaluator evaluator;
            auto rule = aboveRule();
            rule.hysteresis = 2;
            evaluator.onSample(sample(1, 1000, 11), {rule});
            require(evaluator.onSample(sample(2, 1100, 9), {rule}).empty(),
                    "hysteresis must prevent recovery near the threshold");
            auto result = evaluator.onSample(sample(3, 1200, 8), {rule});
            require(result.size() == 1 && result[0].type == AlarmTransitionType::Recovered,
                    "crossing the hysteresis boundary should recover");
        }
        {
            AlarmEvaluator evaluator;
            auto rule = aboveRule();
            rule.cooldownMs = 5000;
            evaluator.onSample(sample(1, 1000, 11), {rule});
            evaluator.onSample(sample(2, 1100, 0), {rule});
            require(evaluator.onSample(sample(3, 2000, 11), {rule}).empty(),
                    "cooldown must suppress an immediate retrigger");
            auto result = evaluator.onSample(sample(4, 6000, 11), {rule});
            require(result.size() == 1 && result[0].type == AlarmTransitionType::Triggered,
                    "rule should trigger again after cooldown expires");
        }
        {
            AlarmEvaluator evaluator;
            auto rule = aboveRule();
            rule.enabled = false;
            require(evaluator.onSample(sample(1, 1000, 100), {rule}).empty(),
                    "disabled rule must not emit transitions");
            rule.enabled = true;
            auto result = evaluator.onSample(sample(2, 2000, 11), {rule});
            require(result.size() == 1, "enabled rule should evaluate normally");
            require(evaluator.onSample(sample(2, 2100, 12), {rule}).empty(),
                    "duplicate sample sequence must be idempotent");
        }
        {
            AlarmEvaluator evaluator;
            auto rule = aboveRule();
            evaluator.onSample(sample(1, 1000, 11), {rule});
            rule.enabled = false;
            require(evaluator.onSample(sample(2, 1100, 11), {rule}).empty(),
                    "disabling an active rule must not emit repeated triggers");
            auto result = evaluator.onSample(sample(3, 1200, 9), {rule});
            require(result.size() == 1 && result[0].type == AlarmTransitionType::Recovered,
                    "disabled rules should still close an active event when it recovers");
        }
        {
            AlarmEvaluator evaluator;
            auto rule = aboveRule();
            rule.direction = ThresholdDirection::Below;
            require(evaluator.onSample(sample(1, 1000, 10.1), {rule}).empty(),
                    "above threshold must not trigger a below rule");
            auto result = evaluator.onSample(sample(2, 2000, 10.0), {rule});
            require(result.size() == 1 && result[0].type == AlarmTransitionType::Triggered,
                    "equality satisfies a below threshold");
        }
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
