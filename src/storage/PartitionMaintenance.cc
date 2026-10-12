#include "PartitionMaintenance.h"

#include <drogon/orm/Exception.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace labscope::storage
{

PartitionMaintenance::PartitionMaintenance(drogon::orm::DbClientPtr dbClient)
    : dbClient_(std::move(dbClient))
{
    if (!dbClient_)
    {
        throw std::invalid_argument("PartitionMaintenance requires a DbClient");
    }
}

void PartitionMaintenance::ensureStartupPartitions(Completion completion) const
{
    execute("SELECT labscope_ensure_telemetry_partitions()", std::move(completion));
}

void PartitionMaintenance::runRetentionMaintenance(Completion completion) const
{
    execute("SELECT labscope_maintain_telemetry()", std::move(completion));
}

void PartitionMaintenance::execute(const char *sql, Completion completion) const
{
    auto callback = std::make_shared<Completion>(std::move(completion));
    dbClient_->execSqlAsync(
        sql,
        [callback](const drogon::orm::Result &) {
            if (*callback)
            {
                auto done = std::move(*callback);
                done(true, {});
            }
        },
        [callback](const drogon::orm::DrogonDbException &error) {
            if (*callback)
            {
                auto done = std::move(*callback);
                done(false, error.base().what());
            }
        });
}

}  // namespace labscope::storage
