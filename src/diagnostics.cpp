#include "diagnostics.h"

#include <cmath>

namespace powerscope {

std::vector<DiagnosticFinding> EvaluateDiagnostics(const ObservationSnapshot& snapshot) {
    std::vector<DiagnosticFinding> findings;
    const double draw = snapshot.battery.system_draw_w;
    if (std::isfinite(draw)) {
        if (draw >= 40.0) {
            findings.push_back({DiagnosticSeverity::Severe, DiagnosticKind::HighSystemDraw, draw, MeasurementUnit::Watts});
        } else if (draw >= 25.0) {
            findings.push_back({DiagnosticSeverity::Attention, DiagnosticKind::HighSystemDraw, draw, MeasurementUnit::Watts});
        } else if (draw >= 17.0) {
            findings.push_back({DiagnosticSeverity::Hint, DiagnosticKind::HighSystemDraw, draw, MeasurementUnit::Watts});
        }
    }
    if (snapshot.gpu.status == DataStatus::Available && std::isfinite(snapshot.gpu.power_w)) {
        if (snapshot.gpu.power_w >= 5.0 && snapshot.gpu.utilization >= 0 && snapshot.gpu.utilization <= 5) {
            findings.push_back({DiagnosticSeverity::Attention, DiagnosticKind::IdleGpuPower,
                snapshot.gpu.power_w, MeasurementUnit::Watts});
        } else if (snapshot.gpu.power_w >= 10.0) {
            findings.push_back({DiagnosticSeverity::Hint, DiagnosticKind::HighGpuPower,
                snapshot.gpu.power_w, MeasurementUnit::Watts});
        }
    }
    if (std::isfinite(snapshot.cpu.utilization) && snapshot.cpu.utilization >= 20.0) {
        findings.push_back({DiagnosticSeverity::Attention, DiagnosticKind::HighCpuUtilization,
            snapshot.cpu.utilization, MeasurementUnit::Percent});
    }
    if (snapshot.display.brightness_percent >= 75) {
        findings.push_back({DiagnosticSeverity::Hint, DiagnosticKind::HighBrightness,
            static_cast<double>(snapshot.display.brightness_percent), MeasurementUnit::Percent});
    }
    if (snapshot.display.refresh_hz >= 120) {
        findings.push_back({DiagnosticSeverity::Hint, DiagnosticKind::HighRefreshRate,
            static_cast<double>(snapshot.display.refresh_hz), MeasurementUnit::Hertz});
    }
    if (snapshot.display.hdr_enabled) {
        findings.push_back({DiagnosticSeverity::Hint, DiagnosticKind::HdrEnabled, 1.0, MeasurementUnit::None});
    }
    const double process_io = snapshot.processes.total_read_mib_s + snapshot.processes.total_write_mib_s;
    if (process_io >= 10.0) {
        findings.push_back({DiagnosticSeverity::Hint, DiagnosticKind::HighProcessIo, process_io, MeasurementUnit::MebibytesPerSecond});
    }
    const double network_io = snapshot.network.receive_mib_s + snapshot.network.send_mib_s;
    if (network_io >= 5.0) {
        findings.push_back({DiagnosticSeverity::Hint, DiagnosticKind::HighNetworkTraffic, network_io, MeasurementUnit::MebibytesPerSecond});
    }
    return findings;
}

} // namespace powerscope
