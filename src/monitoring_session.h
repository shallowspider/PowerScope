#pragma once

#include "powerscope_types.h"

#include <chrono>
#include <functional>

namespace powerscope {

using SessionClock = std::chrono::steady_clock;

class ObservationSource {
public:
    virtual ~ObservationSource() = default;
    virtual void Prime() = 0;
    virtual ObservationSnapshot SampleFast(double elapsed_seconds) = 0;
    virtual GpuReading SampleGpu() = 0;
};

class SessionRuntime {
public:
    virtual ~SessionRuntime() = default;
    virtual SessionClock::time_point Now() = 0;
    virtual bool StopRequested() const = 0;
    virtual void WaitUntil(SessionClock::time_point deadline) = 0;
};

class MonitoringSession {
public:
    MonitoringSession(Options options, ObservationSource& source, SessionRuntime& runtime);
    void Run(const std::function<void(const ObservationSnapshot&)>& observe);

private:
    Options options_;
    ObservationSource& source_;
    SessionRuntime& runtime_;
};

} // namespace powerscope
