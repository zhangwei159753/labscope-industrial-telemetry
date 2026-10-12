#pragma once

namespace labscope::ingest
{

class TelemetrySource
{
  public:
    virtual ~TelemetrySource() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
};

}  // namespace labscope::ingest
