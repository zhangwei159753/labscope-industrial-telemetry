#pragma once

#include "AlarmEvaluator.h"

#include <drogon/orm/DbClient.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace labscope::alarms
{

struct AlarmProcessorState;

class AlarmProcessor final
{
  public:
    explicit AlarmProcessor(drogon::orm::DbClientPtr dbClient);
    ~AlarmProcessor();
    AlarmProcessor(const AlarmProcessor &) = delete;
    AlarmProcessor &operator=(const AlarmProcessor &) = delete;

    void start();
    void stop();
    bool isRunning() const noexcept;

  private:
    void run();
    void processOne();

    drogon::orm::DbClientPtr dbClient_;
    std::shared_ptr<AlarmProcessorState> state_;
    std::thread thread_;
};

}  // namespace labscope::alarms
