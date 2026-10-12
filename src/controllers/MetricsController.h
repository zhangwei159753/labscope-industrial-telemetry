#pragma once

#include <drogon/orm/DbClient.h>

namespace labscope::ingest { class IngestionService; }
namespace labscope::ingest { class MqttConsumer; }
namespace labscope::alarms { class AlarmProcessor; }

namespace labscope::api
{

class MetricsController final
{
  public:
    static void registerRoutes(drogon::orm::DbClientPtr dbClient,
                               labscope::ingest::IngestionService *ingestion,
                               labscope::ingest::MqttConsumer *mqtt,
                               labscope::alarms::AlarmProcessor *alarms);
};

}  // namespace labscope::api
