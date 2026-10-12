#pragma once

#include "AlarmRule.h"

#include <drogon/orm/DbClient.h>

#include <functional>
#include <memory>
#include <string>

namespace labscope::alarms
{

struct CreateRule
{
    std::string siteId;
    std::string deviceId;
    std::string metric;
    ThresholdDirection direction{ThresholdDirection::Above};
    double threshold{0.0};
    int64_t durationMs{0};
    double hysteresis{0.0};
    int64_t cooldownMs{0};
    AlarmSeverity severity{AlarmSeverity::Warning};
};

struct CreateRuleResult
{
    bool succeeded{false};
    int64_t ruleId{0};
    std::string error;
};

struct AcknowledgeResult
{
    bool succeeded{false};
    bool found{false};
    std::string error;
};

class AlarmRepository final
{
  public:
    using AcknowledgeCallback = std::function<void(AcknowledgeResult)>;
    using CreateRuleCallback = std::function<void(CreateRuleResult)>;

    explicit AlarmRepository(drogon::orm::DbClientPtr dbClient);
    void createRule(const CreateRule &rule, int64_t userId,
                    CreateRuleCallback callback) const;
    void acknowledge(int64_t eventId,
                     const std::string &siteId,
                     int64_t userId,
                     const std::string &remark,
                     AcknowledgeCallback callback) const;

  private:
    drogon::orm::DbClientPtr dbClient_;
};

}  // namespace labscope::alarms
