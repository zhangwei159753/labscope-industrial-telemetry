#pragma once

#include <drogon/orm/DbClient.h>

#include <functional>
#include <string>

namespace labscope::storage
{

class PartitionMaintenance final
{
  public:
    using Completion = std::function<void(bool succeeded, std::string error)>;

    explicit PartitionMaintenance(drogon::orm::DbClientPtr dbClient);

    // Call after Flyway has completed and before accepting device messages.
    void ensureStartupPartitions(Completion completion) const;

    // Call on a low-frequency maintenance timer; the SQL routine is idempotent.
    void runRetentionMaintenance(Completion completion) const;

  private:
    void execute(const char *sql, Completion completion) const;

    drogon::orm::DbClientPtr dbClient_;
};

}  // namespace labscope::storage
