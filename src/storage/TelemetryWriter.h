#pragma once

#include "TelemetryRepository.h"
#include "../ingest/IngestionService.h"

#include <memory>
#include <functional>

namespace labscope::storage
{

class TelemetryWriter final
{
  public:
    using CommittedCallback =
        std::function<void(const telemetry::TelemetryEnvelope &)>;

    explicit TelemetryWriter(std::shared_ptr<TelemetryRepository> repository,
                             CommittedCallback committed = {});

    void persist(std::vector<ingest::QueuedTelemetry> batch,
                 ingest::BatchCompletionCallback completion) const;

  private:
    std::shared_ptr<TelemetryRepository> repository_;
    CommittedCallback committed_;
};

}  // namespace labscope::storage
