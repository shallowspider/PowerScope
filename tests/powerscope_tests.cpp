#include "diagnostics.h"
#include "dashboard.h"
#include "monitoring_session.h"

#include <iostream>
#include <string_view>
#include <algorithm>
#include <sstream>
#include <deque>

namespace {

int failures = 0;

void Expect(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void SevereDrawProducesStructuredFinding() {
    powerscope::ObservationSnapshot snapshot;
    snapshot.battery.system_draw_w = 42.0;

    const auto findings = powerscope::EvaluateDiagnostics(snapshot);

    Expect(findings.size() == 1, "42 W should produce exactly one finding");
    if (findings.size() == 1) {
        Expect(findings[0].kind == powerscope::DiagnosticKind::HighSystemDraw,
            "42 W should be classified as high system draw");
        Expect(findings[0].severity == powerscope::DiagnosticSeverity::Severe,
            "42 W should be severe");
        Expect(findings[0].observed_value == 42.0,
            "finding should retain the observed value");
        Expect(findings[0].unit == powerscope::MeasurementUnit::Watts,
            "finding should declare the observed value unit");
    }
}

void ElevatedDrawPreservesExistingThresholds() {
    powerscope::ObservationSnapshot snapshot;
    snapshot.battery.system_draw_w = 25.0;

    const auto findings = powerscope::EvaluateDiagnostics(snapshot);

    Expect(findings.size() == 1, "25 W should produce one finding");
    if (findings.size() == 1) {
        Expect(findings[0].severity == powerscope::DiagnosticSeverity::Attention,
            "25 W should require attention");
    }
}

void ActivityFactorsProduceIndependentFindings() {
    powerscope::ObservationSnapshot snapshot;
    snapshot.gpu.status = powerscope::DataStatus::Available;
    snapshot.gpu.power_w = 6.0;
    snapshot.gpu.utilization = 2;
    snapshot.cpu.utilization = 20.0;
    snapshot.display.brightness_percent = 75;
    snapshot.display.refresh_hz = 120;
    snapshot.display.hdr_enabled = true;
    snapshot.processes.total_read_mib_s = 7.0;
    snapshot.processes.total_write_mib_s = 3.0;
    snapshot.network.receive_mib_s = 4.0;
    snapshot.network.send_mib_s = 1.0;

    const auto findings = powerscope::EvaluateDiagnostics(snapshot);
    const auto has = [&](powerscope::DiagnosticKind kind) {
        return std::ranges::any_of(findings, [=](const auto& finding) { return finding.kind == kind; });
    };

    Expect(findings.size() == 7, "independent activity factors should all be retained");
    Expect(has(powerscope::DiagnosticKind::IdleGpuPower), "idle GPU power should be diagnosed");
    Expect(has(powerscope::DiagnosticKind::HighCpuUtilization), "high CPU should be diagnosed");
    Expect(has(powerscope::DiagnosticKind::HighBrightness), "high brightness should be diagnosed");
    Expect(has(powerscope::DiagnosticKind::HighRefreshRate), "high refresh should be diagnosed");
    Expect(has(powerscope::DiagnosticKind::HdrEnabled), "HDR should be diagnosed");
    Expect(has(powerscope::DiagnosticKind::HighProcessIo), "process I/O should be diagnosed");
    Expect(has(powerscope::DiagnosticKind::HighNetworkTraffic), "network traffic should be diagnosed");
}

void DashboardSelectsLayoutWithHysteresis() {
    const powerscope::ObservationSnapshot snapshot;
    const std::vector<powerscope::DiagnosticFinding> findings;

    const auto wide = powerscope::BuildDashboard(snapshot, findings, {110, 30, std::nullopt});
    const auto vertical = powerscope::BuildDashboard(snapshot, findings, {109, 30, std::nullopt});
    const auto retained_wide = powerscope::BuildDashboard(
        snapshot, findings, {106, 30, powerscope::DashboardLayout::Wide});
    const auto dropped_wide = powerscope::BuildDashboard(
        snapshot, findings, {105, 30, powerscope::DashboardLayout::Wide});

    Expect(wide.layout == powerscope::DashboardLayout::Wide, "110 columns should use wide layout");
    Expect(vertical.layout == powerscope::DashboardLayout::Vertical,
        "109 columns should use vertical layout");
    Expect(retained_wide.layout == powerscope::DashboardLayout::Wide,
        "wide layout should survive inside the four-column buffer");
    Expect(dropped_wide.layout == powerscope::DashboardLayout::Vertical,
        "wide layout should drop below the four-column buffer");
}

std::wstring FrameText(const powerscope::DashboardFrame& frame) {
    std::wostringstream text;
    for (const auto& line : frame.lines) text << powerscope::PlainText(line) << L'\n';
    return text.str();
}

void VerticalDashboardGroupsInformationAndFitsHeight() {
    powerscope::ObservationSnapshot snapshot;
    snapshot.timestamp = L"2026-07-16 17:30:00";
    snapshot.battery.ac_online = true;
    snapshot.battery.draw_status = powerscope::DataStatus::Unmeasurable;
    for (std::uint32_t pid = 1; pid <= 6; ++pid) {
        snapshot.processes.top.push_back({pid, L"worker.exe", 3.0, 1.0, 0.5});
    }
    const std::vector findings{
        powerscope::DiagnosticFinding{powerscope::DiagnosticSeverity::Attention,
            powerscope::DiagnosticKind::HighCpuUtilization, 25.0},
    };

    const auto frame = powerscope::BuildDashboard(snapshot, findings, {80, 22, std::nullopt});
    const std::wstring text = FrameText(frame);

    Expect(frame.lines.size() <= 22, "vertical dashboard should fit the terminal height");
    Expect(text.find(L"整机功耗") < text.find(L"电池与电源"), "power should precede battery");
    Expect(text.find(L"电池与电源") < text.find(L"核心资源"), "battery should precede resources");
    Expect(text.find(L"核心资源") < text.find(L"显示与网络"), "resources should precede display");
    Expect(text.find(L"显示与网络") < text.find(L"活跃进程"), "display should precede processes");
    Expect(text.find(L"活跃进程") < text.find(L"自动诊断"), "processes should precede diagnosis");
    Expect(text.find(L"不可测量") != std::wstring::npos, "missing power should explain why");
    Expect(text.find(L"未显示") != std::wstring::npos, "trimmed processes should be disclosed");
}

void VeryShortTerminalKeepsOneProcessAndDiagnosis() {
    powerscope::ObservationSnapshot snapshot;
    snapshot.battery.draw_status = powerscope::DataStatus::Available;
    snapshot.battery.system_draw_w = 18.0;
    for (std::uint32_t pid = 1; pid <= 3; ++pid)
        snapshot.processes.top.push_back({pid, L"worker.exe", 2.0, 0.0, 0.0});
    const std::vector findings{
        powerscope::DiagnosticFinding{powerscope::DiagnosticSeverity::Hint,
            powerscope::DiagnosticKind::HighSystemDraw, 18.0},
    };

    const auto frame = powerscope::BuildDashboard(snapshot, findings, {70, 16, std::nullopt});
    const auto text = FrameText(frame);

    Expect(frame.lines.size() <= 16, "very short dashboard should stay within sixteen rows");
    Expect(text.find(L"worker.exe") != std::wstring::npos, "very short dashboard should retain one process");
    Expect(text.find(L"自动诊断") != std::wstring::npos, "very short dashboard should retain diagnosis");
    Expect(text.find(L"未显示") != std::wstring::npos, "very short dashboard should disclose hidden processes");
}

int TestDisplayWidth(const std::wstring& text) {
    int width = 0;
    for (wchar_t ch : text) {
        const auto value = static_cast<unsigned int>(ch);
        const bool wide = (value >= 0x1100 && value <= 0x115F) ||
            (value >= 0x2E80 && value <= 0xA4CF) || (value >= 0xAC00 && value <= 0xD7A3) ||
            (value >= 0xF900 && value <= 0xFAFF) || (value >= 0xFE10 && value <= 0xFE6F) ||
            (value >= 0xFF01 && value <= 0xFF60) || (value >= 0xFFE0 && value <= 0xFFE6);
        width += wide ? 2 : 1;
    }
    return width;
}

void ShortNarrowDashboardKeepsKeyContentWithinBothLimits() {
    powerscope::ObservationSnapshot snapshot;
    snapshot.timestamp = L"2026-07-17 10:00:00";
    snapshot.power_scheme = L"一个很长但不应挤掉关键字段的电源方案名称";
    snapshot.battery.draw_status = powerscope::DataStatus::Available;
    snapshot.battery.system_draw_w = 20.0;
    snapshot.power_averages = {19.0, 18.0, 17.0, 45};
    snapshot.cpu.status = powerscope::DataStatus::TemporarilyUnavailable;
    snapshot.memory.status = powerscope::DataStatus::Unmeasurable;
    snapshot.display.status = powerscope::DataStatus::TemporarilyUnavailable;
    snapshot.network.status = powerscope::DataStatus::TemporarilyUnavailable;
    for (std::uint32_t pid = 1; pid <= 3; ++pid)
        snapshot.processes.top.push_back({pid, L"a-very-long-background-process-name.exe", 2.0, 1.0, 0.5});
    std::vector<powerscope::DiagnosticFinding> findings(5,
        {powerscope::DiagnosticSeverity::Hint, powerscope::DiagnosticKind::HighRefreshRate,
            120.0, powerscope::MeasurementUnit::Hertz});

    const auto frame = powerscope::BuildDashboard(snapshot, findings, {50, 16, std::nullopt});
    const auto wide_but_short = powerscope::BuildDashboard(
        snapshot, findings, {120, 16, powerscope::DashboardLayout::Wide});
    const auto minimum_width = powerscope::BuildDashboard(snapshot, findings, {20, 16, std::nullopt});
    const auto text = FrameText(frame);

    Expect(frame.layout == powerscope::DashboardLayout::Vertical,
        "short terminal should use compact vertical layout even when previously wide");
    Expect(wide_but_short.layout == powerscope::DashboardLayout::Vertical &&
        wide_but_short.lines.size() <= 16,
        "wide but short terminal should also fall back to compact vertical layout");
    Expect(minimum_width.lines.size() <= 16, "minimum-width dashboard should fit sixteen rows");
    for (const auto& line : minimum_width.lines)
        Expect(TestDisplayWidth(powerscope::PlainText(line)) <= 19,
            "minimum-width compact fields should fit without generic truncation");
    const auto minimum_text = FrameText(minimum_width);
    Expect(minimum_text.find(L"CPU") != std::wstring::npos && minimum_text.find(L"RAM") != std::wstring::npos,
        "minimum-width dashboard should retain both CPU and memory");
    Expect(frame.lines.size() <= 16, "short narrow dashboard should fit sixteen rows");
    for (const auto& line : frame.lines)
        Expect(TestDisplayWidth(powerscope::PlainText(line)) <= 49,
            "short narrow dashboard should fit before renderer truncation");
    Expect(text.find(L"5秒") != std::wstring::npos && text.find(L"30秒") != std::wstring::npos &&
        text.find(L"60秒") != std::wstring::npos, "short narrow dashboard should retain all averages");
    Expect(text.find(L"当前不可用") != std::wstring::npos,
        "short narrow dashboard should retain a missing-data reason");
    Expect(text.find(L"未显示") != std::wstring::npos,
        "short narrow dashboard should disclose hidden processes or diagnostics");
}

void NarrowDashboardWrapsInsteadOfTruncating() {
    powerscope::ObservationSnapshot snapshot;
    snapshot.timestamp = L"2026-07-17 10:00:00";
    snapshot.power_scheme = L"最佳能效";
    snapshot.battery.draw_status = powerscope::DataStatus::Available;
    snapshot.battery.system_draw_w = 20.0;
    snapshot.power_averages = {19.0, 18.0, 17.0, 60};
    snapshot.cpu.status = powerscope::DataStatus::Available;
    snapshot.memory.status = powerscope::DataStatus::Available;
    snapshot.display.status = powerscope::DataStatus::Available;
    snapshot.display.brightness_status = powerscope::DataStatus::Available;
    for (std::uint32_t pid = 1; pid <= 6; ++pid)
        snapshot.processes.top.push_back({pid, L"background-worker.exe", 2.0, 1.0, 0.5});
    std::vector<powerscope::DiagnosticFinding> findings;
    for (int i = 0; i < 5; ++i)
        findings.push_back({powerscope::DiagnosticSeverity::Hint,
            powerscope::DiagnosticKind::HighRefreshRate, 120.0, powerscope::MeasurementUnit::Hertz});

    const auto frame = powerscope::BuildDashboard(snapshot, findings, {50, 40, std::nullopt});
    const auto text = FrameText(frame);

    Expect(frame.lines.size() <= 40, "narrow dashboard should still fit its height budget");
    for (const auto& line : frame.lines)
        Expect(TestDisplayWidth(powerscope::PlainText(line)) <= 49,
            "narrow dashboard should wrap before the renderer must truncate");
    Expect(text.find(L"5秒") != std::wstring::npos && text.find(L"30秒") != std::wstring::npos &&
        text.find(L"60秒") != std::wstring::npos, "narrow dashboard should retain every power average");
    Expect(text.find(L"电源方案") != std::wstring::npos, "narrow dashboard should retain the power scheme");
    Expect(text.find(L"另有") != std::wstring::npos, "narrow dashboard should disclose folded content");
}

void MissingReadingsExplainTheirStatus() {
    powerscope::ObservationSnapshot snapshot;
    snapshot.cpu.status = powerscope::DataStatus::TemporarilyUnavailable;
    snapshot.memory.status = powerscope::DataStatus::Unmeasurable;
    snapshot.display.status = powerscope::DataStatus::Available;
    snapshot.display.brightness_status = powerscope::DataStatus::Unmeasurable;
    snapshot.battery.estimate_status = powerscope::DataStatus::Unmeasurable;
    snapshot.gpu.status = powerscope::DataStatus::Available;
    snapshot.network.status = powerscope::DataStatus::TemporarilyUnavailable;

    const auto text = FrameText(powerscope::BuildDashboard(snapshot, {}, {100, 40, std::nullopt}));

    Expect(text.find(L"CPU  —（当前不可用）") != std::wstring::npos,
        "CPU should explain temporary unavailability");
    Expect(text.find(L"内存  —（不可测量）") != std::wstring::npos,
        "memory should explain an unmeasurable state");
    Expect(text.find(L"亮度 —（不可测量）") != std::wstring::npos,
        "brightness should explain an unmeasurable state");
    Expect(text.find(L"HDR —（当前不可用）") != std::wstring::npos,
        "HDR should not report off when its query is unavailable");
    Expect(text.find(L"预计 —（不可测量）") != std::wstring::npos,
        "battery estimate should explain an unmeasurable state");
    Expect(text.find(L"利用率 —（当前不可用）") != std::wstring::npos,
        "missing GPU sensors should explain temporary unavailability");
    Expect(text.find(L"网络  —（当前不可用）") != std::wstring::npos,
        "network should explain temporary unavailability");
}

class ScriptedSource final : public powerscope::ObservationSource {
public:
    explicit ScriptedSource(std::deque<powerscope::ObservationSnapshot> readings)
        : readings_(std::move(readings)) {}

    void Prime() override { primed = true; }

    powerscope::ObservationSnapshot SampleFast(double elapsed) override {
        elapsed_samples.push_back(elapsed);
        auto reading = readings_.front();
        readings_.pop_front();
        return reading;
    }

    powerscope::GpuReading SampleGpu() override {
        ++gpu_samples;
        powerscope::GpuReading reading;
        reading.status = powerscope::DataStatus::Available;
        reading.power_w = static_cast<double>(gpu_samples);
        return reading;
    }

    bool primed = false;
    int gpu_samples = 0;
    std::vector<double> elapsed_samples;

private:
    std::deque<powerscope::ObservationSnapshot> readings_;
};

class ScriptedRuntime final : public powerscope::SessionRuntime {
public:
    explicit ScriptedRuntime(int cycles) : stop_after_waits_(cycles + 1) {}

    powerscope::SessionClock::time_point Now() override { return now_; }
    bool StopRequested() const override { return waits_ >= stop_after_waits_; }
    void WaitUntil(powerscope::SessionClock::time_point deadline) override {
        now_ = deadline;
        ++waits_;
    }

private:
    powerscope::SessionClock::time_point now_{};
    int waits_ = 0;
    int stop_after_waits_ = 0;
};

void MonitoringSessionOwnsCadenceAndGpuCache() {
    std::deque<powerscope::ObservationSnapshot> readings(7);
    for (auto& reading : readings) {
        reading.battery.draw_status = powerscope::DataStatus::Available;
        reading.battery.system_draw_w = 20.0;
    }
    ScriptedSource source(std::move(readings));
    ScriptedRuntime runtime(7);
    powerscope::Options options;
    options.interval_ms = 1000;
    options.gpu_interval_ms = 5000;
    powerscope::MonitoringSession session(options, source, runtime);
    std::vector<powerscope::ObservationSnapshot> observed;

    session.Run([&](const auto& snapshot) { observed.push_back(snapshot); });

    Expect(source.primed, "monitoring session should prime stateful probes");
    Expect(observed.size() == 7, "monitoring session should own the full refresh loop");
    Expect(source.gpu_samples == 2, "GPU should be sampled immediately and then every five seconds");
    if (observed.size() == 7) {
        Expect(observed[0].gpu.power_w == 1.0 && observed[4].gpu.power_w == 1.0,
            "GPU reading should be cached between GPU sampling ticks");
        Expect(observed[5].gpu.power_w == 2.0,
            "GPU cache should refresh on the configured tick");
    }
}

void AcTransitionResetsPowerHistory() {
    std::deque<powerscope::ObservationSnapshot> readings(4);
    readings[0].battery = {.ac_online = false, .system_draw_w = 10.0,
        .draw_status = powerscope::DataStatus::Available};
    readings[1].battery = {.ac_online = false, .system_draw_w = 20.0,
        .draw_status = powerscope::DataStatus::Available};
    readings[2].battery = {.ac_online = true, .draw_status = powerscope::DataStatus::Unmeasurable};
    readings[3].battery = {.ac_online = false, .system_draw_w = 30.0,
        .draw_status = powerscope::DataStatus::Available};
    ScriptedSource source(std::move(readings));
    ScriptedRuntime runtime(4);
    powerscope::MonitoringSession session({}, source, runtime);
    std::vector<powerscope::ObservationSnapshot> observed;

    session.Run([&](const auto& snapshot) { observed.push_back(snapshot); });

    Expect(observed.size() == 4, "four scripted cycles should be observed");
    if (observed.size() == 4) {
        Expect(observed[1].power_averages.seconds_5 == 15.0,
            "power history should average battery samples");
        Expect(observed[3].power_averages.seconds_5 == 30.0,
            "connecting AC should reset prior battery history");
    }
}

void PowerHistoryMaintainsAllWindowsAndPrunesOldSamples() {
    std::deque<powerscope::ObservationSnapshot> readings(62);
    for (auto& reading : readings) {
        reading.battery.draw_status = powerscope::DataStatus::Available;
        reading.battery.system_draw_w = 20.0;
    }
    readings.front().battery.system_draw_w = 10.0;
    ScriptedSource source(std::move(readings));
    ScriptedRuntime runtime(62);
    powerscope::Options options;
    options.gpu_enabled = false;
    powerscope::MonitoringSession session(options, source, runtime);
    std::vector<powerscope::ObservationSnapshot> observed;

    session.Run([&](const auto& snapshot) { observed.push_back(snapshot); });

    Expect(observed.size() == 62, "sixty-two history samples should be observed");
    if (observed.size() == 62) {
        const auto& final = observed.back().power_averages;
        Expect(final.seconds_5 == 20.0, "five-second average should contain recent samples");
        Expect(final.seconds_30 == 20.0, "thirty-second average should contain recent samples");
        Expect(final.seconds_60 == 20.0, "sixty-second average should prune the oldest sample");
        Expect(final.coverage_seconds == 60, "history coverage should cap at sixty seconds");
    }
}

class OverrunRuntime final : public powerscope::SessionRuntime {
public:
    powerscope::SessionClock::time_point Now() override {
        ++now_calls_;
        if (now_calls_ == 4) now_ += std::chrono::milliseconds(1500);
        return now_;
    }
    bool StopRequested() const override { return waits.size() >= 3; }
    void WaitUntil(powerscope::SessionClock::time_point deadline) override {
        waits.push_back(deadline);
        if (deadline > now_) now_ = deadline;
    }
    std::vector<powerscope::SessionClock::time_point> waits;
private:
    powerscope::SessionClock::time_point now_{};
    int now_calls_ = 0;
};

void SamplingOverrunResetsTheNextDeadline() {
    std::deque<powerscope::ObservationSnapshot> readings(2);
    ScriptedSource source(std::move(readings));
    OverrunRuntime runtime;
    powerscope::MonitoringSession session({}, source, runtime);

    session.Run([](const auto&) {});

    Expect(runtime.waits.size() == 3, "two cycles should produce priming and two cycle waits");
    if (runtime.waits.size() == 3) {
        Expect(runtime.waits[1] - runtime.waits[0] == std::chrono::milliseconds(1500),
            "an overrun should reset the missed deadline to now");
        Expect(runtime.waits[2] - runtime.waits[1] == std::chrono::milliseconds(1000),
            "sampling should resume at the configured cadence after an overrun");
    }
    Expect(source.elapsed_samples.size() == 2 && source.elapsed_samples[1] == 1.5,
        "elapsed time should reflect the overrun instead of a catch-up tick");
}

} // namespace

int main() {
    SevereDrawProducesStructuredFinding();
    ElevatedDrawPreservesExistingThresholds();
    ActivityFactorsProduceIndependentFindings();
    DashboardSelectsLayoutWithHysteresis();
    VerticalDashboardGroupsInformationAndFitsHeight();
    VeryShortTerminalKeepsOneProcessAndDiagnosis();
    ShortNarrowDashboardKeepsKeyContentWithinBothLimits();
    NarrowDashboardWrapsInsteadOfTruncating();
    MissingReadingsExplainTheirStatus();
    MonitoringSessionOwnsCadenceAndGpuCache();
    AcTransitionResetsPowerHistory();
    PowerHistoryMaintainsAllWindowsAndPrunesOldSamples();
    SamplingOverrunResetsTheNextDeadline();
    if (failures == 0) std::cout << "All tests passed\n";
    return failures == 0 ? 0 : 1;
}
