#pragma once

#include "powerscope_types.h"

#include <vector>

namespace powerscope {

enum class DiagnosticSeverity { Hint, Attention, Severe };
enum class MeasurementUnit { None, Watts, Percent, Hertz, MebibytesPerSecond };

enum class DiagnosticKind {
    HighSystemDraw,
    IdleGpuPower,
    HighGpuPower,
    HighCpuUtilization,
    HighBrightness,
    HighRefreshRate,
    HdrEnabled,
    HighProcessIo,
    HighNetworkTraffic,
};

struct DiagnosticFinding {
    DiagnosticSeverity severity;
    DiagnosticKind kind;
    double observed_value = 0.0;
    MeasurementUnit unit = MeasurementUnit::None;
};

std::vector<DiagnosticFinding> EvaluateDiagnostics(const ObservationSnapshot& snapshot);

} // namespace powerscope
