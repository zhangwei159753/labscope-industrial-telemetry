#include "DeviceRepository.h"

#include <drogon/orm/Exception.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace labscope::storage
{

DeviceRepository::DeviceRepository(drogon::orm::DbClientPtr dbClient)
    : dbClient_(std::move(dbClient))
{
    if (!dbClient_)
        throw std::invalid_argument("DeviceRepository requires a DbClient");
}

void DeviceRepository::reserveSourceSequence(
    const std::string &siteId,
    const std::string &deviceId,
    SequenceReservationCallback completion) const
{
    auto callback = std::make_shared<SequenceReservationCallback>(std::move(completion));
    dbClient_->execSqlAsync(
        "UPDATE devices "
        "SET next_source_seq = GREATEST(next_source_seq, last_seq + 1) + 1, "
        "    updated_at = clock_timestamp() "
        "WHERE site_id = $1 AND device_id = $2 AND enabled "
        "RETURNING next_source_seq - 1 AS reserved_seq",
        [callback](const drogon::orm::Result &result) {
            if (*callback)
            {
                auto done = std::move(*callback);
                if (result.empty())
                    done(std::nullopt, "device is missing or disabled");
                else
                    done(result[0]["reserved_seq"].as<int64_t>(), {});
            }
        },
        [callback](const drogon::orm::DrogonDbException &error) {
            if (*callback)
            {
                auto done = std::move(*callback);
                done(std::nullopt, error.base().what());
            }
        },
        siteId,
        deviceId);
}

}  // namespace labscope::storage
