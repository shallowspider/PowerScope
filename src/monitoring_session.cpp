#include "monitoring_session.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>

namespace powerscope {
namespace {

class PowerHistory {
public:
    void Reset() { values_.clear(); }

    void Add(SessionClock::time_point now, double value) {
        if (std::isfinite(value)) values_.emplace_back(now, value);
        Prune(now);
    }

    double Average(SessionClock::time_point now, std::chrono::seconds window) const {
        double total = 0.0;
        std::size_t count = 0;
        const auto threshold = now - window;
        for (auto it = values_.rbegin(); it != values_.rend(); ++it) {
            if (it->first < threshold) break;
            total += it->second;
            ++count;
        }
        return count == 0 ? MissingNumber() : total / static_cast<double>(count);
    }

    int CoverageSeconds(SessionClock::time_point now) const {
        if (values_.empty()) return 0;
        return static_cast<int>(std::clamp(
            std::chrono::duration_cast<std::chrono::seconds>(now - values_.front().first).count(),
            0LL, 60LL));
    }

private:
    void Prune(SessionClock::time_point now) {
        const auto threshold = now - std::chrono::seconds(60);
        while (!values_.empty() && values_.front().first < threshold) values_.pop_front();
    }

    std::deque<std::pair<SessionClock::time_point, double>> values_;
};

} // namespace

MonitoringSession::MonitoringSession(Options options, ObservationSource& source, SessionRuntime& runtime)
    : options_(options), source_(source), runtime_(runtime) {}

void MonitoringSession::Run(const std::function<void(const ObservationSnapshot&)>& observe) {
    source_.Prime();
    const auto interval = std::chrono::milliseconds(options_.interval_ms);
    const auto gpu_interval = std::chrono::milliseconds(options_.gpu_interval_ms);

    auto last_sample = runtime_.Now();
    runtime_.WaitUntil(last_sample + interval);
    last_sample = runtime_.Now();
    auto next_tick = last_sample;
    std::optional<SessionClock::time_point> last_gpu_sample;
    GpuReading cached_gpu;
    cached_gpu.status = options_.gpu_enabled ? DataStatus::WarmingUp : DataStatus::Disabled;
    PowerHistory power_history;
    bool was_on_ac = false;

    while (!runtime_.StopRequested()) {
        next_tick += interval;
        const auto cycle_start = runtime_.Now();
        double elapsed = std::chrono::duration<double>(cycle_start - last_sample).count();
        if (elapsed <= 0.0) elapsed = options_.interval_ms / 1000.0;
        last_sample = cycle_start;

        ObservationSnapshot snapshot = source_.SampleFast(elapsed);
        snapshot.options = options_;
        if (options_.gpu_enabled && (!last_gpu_sample || cycle_start - *last_gpu_sample >= gpu_interval)) {
            cached_gpu = source_.SampleGpu();
            last_gpu_sample = cycle_start;
        }
        snapshot.gpu = cached_gpu;

        if (snapshot.battery.ac_online && !was_on_ac) power_history.Reset();
        was_on_ac = snapshot.battery.ac_online;
        if (!snapshot.battery.ac_online) {
            power_history.Add(cycle_start, snapshot.battery.system_draw_w);
        }
        snapshot.power_averages.seconds_5 = power_history.Average(cycle_start, std::chrono::seconds(5));
        snapshot.power_averages.seconds_30 = power_history.Average(cycle_start, std::chrono::seconds(30));
        snapshot.power_averages.seconds_60 = power_history.Average(cycle_start, std::chrono::seconds(60));
        snapshot.power_averages.coverage_seconds = power_history.CoverageSeconds(cycle_start);
        observe(snapshot);

        const auto now = runtime_.Now();
        if (next_tick <= now) next_tick = now;
        runtime_.WaitUntil(next_tick);
    }
}

} // namespace powerscope
