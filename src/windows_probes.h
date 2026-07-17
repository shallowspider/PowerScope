#pragma once

#include "monitoring_session.h"

#include <atomic>
#include <memory>

namespace powerscope {

class WindowsObservationSource final : public ObservationSource {
public:
    WindowsObservationSource();
    ~WindowsObservationSource() override;
    WindowsObservationSource(const WindowsObservationSource&) = delete;
    WindowsObservationSource& operator=(const WindowsObservationSource&) = delete;

    void Prime() override;
    ObservationSnapshot SampleFast(double elapsed_seconds) override;
    GpuReading SampleGpu() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class WindowsSessionRuntime final : public SessionRuntime {
public:
    explicit WindowsSessionRuntime(const std::atomic_bool& stop);
    SessionClock::time_point Now() override;
    bool StopRequested() const override;
    void WaitUntil(SessionClock::time_point deadline) override;

private:
    const std::atomic_bool& stop_;
};

} // namespace powerscope
