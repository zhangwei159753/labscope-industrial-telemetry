#pragma once

#include "../ingest/IngestionTypes.h"

#include <drogon/orm/DbClient.h>

#include <functional>
#include <memory>
#include <vector>

namespace labscope::storage
{

class TelemetryRepository final
{
  public:
    using DbClientPtr = drogon::orm::DbClientPtr;
    using BatchCompletionCallback =
        std::function<void(std::vector<ingest::IngestionResult>)>;

    explicit TelemetryRepository(DbClientPtr dbClient);

    // A batch is committed atomically. Results are reported only after commit.
    void insertBatch(std::vector<ingest::QueuedTelemetry> batch,
                     BatchCompletionCallback completion) const;

  private:
    DbClientPtr dbClient_;
};

}  // namespace labscope::storage
