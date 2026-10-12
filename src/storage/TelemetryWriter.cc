#include "TelemetryWriter.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace labscope::storage
{

TelemetryWriter::TelemetryWriter(std::shared_ptr<TelemetryRepository> repository,
                                 CommittedCallback committed)
    : repository_(std::move(repository)), committed_(std::move(committed))
{
    if (!repository_)
        throw std::invalid_argument("TelemetryWriter requires a repository");
}

void TelemetryWriter::persist(std::vector<ingest::QueuedTelemetry> batch,
                              ingest::BatchCompletionCallback completion) const
{
    auto committed = committed_;
    auto envelopes = std::make_shared<std::vector<telemetry::TelemetryEnvelope>>();
    envelopes->reserve(batch.size());
    for (const auto &item : batch)
        envelopes->push_back(item.envelope);
    repository_->insertBatch(
        std::move(batch),
        [committed = std::move(committed), envelopes,
         completion = std::move(completion)](std::vector<ingest::IngestionResult> results) mutable {
            const auto count = (std::min)(results.size(), envelopes->size());
            if (committed)
            {
                for (std::size_t index = 0; index < count; ++index)
                {
                    const auto status = results[index].status;
                    if (status == ingest::IngestionStatus::Persisted ||
                        status == ingest::IngestionStatus::Duplicate)
                    {
                        // Updating the in-memory monitor is a best-effort side effect.
                        // It must never prevent the durable commit result from reaching
                        // the MQTT ACK completion callback.
                        try
                        {
                            committed((*envelopes)[index]);
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }
            if (completion)
                completion(std::move(results));
        });
}

}  // namespace labscope::storage
