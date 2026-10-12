#pragma once

#include <drogon/orm/DbClient.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace labscope::storage
{

class DeviceRepository final
{
  public:
    using SequenceReservationCallback =
        std::function<void(std::optional<int64_t>, std::string)>;

    explicit DeviceRepository(drogon::orm::DbClientPtr dbClient);

    void reserveSourceSequence(const std::string &siteId,
                               const std::string &deviceId,
                               SequenceReservationCallback completion) const;

  private:
    drogon::orm::DbClientPtr dbClient_;
};

}  // namespace labscope::storage
