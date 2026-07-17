#pragma once

#include "diagnostics.h"

#include <optional>
#include <string>
#include <vector>

namespace powerscope {

enum class TextTone { Default, Key, Muted, Hint, Attention, Severe };
enum class DashboardLayout { Wide, Vertical };

struct TextSpan {
    std::wstring text;
    TextTone tone = TextTone::Default;
};

using StyledLine = std::vector<TextSpan>;

struct DashboardFrame {
    DashboardLayout layout = DashboardLayout::Vertical;
    std::vector<StyledLine> lines;
};

struct DashboardConstraints {
    int width = 100;
    int height = 30;
    std::optional<DashboardLayout> previous_layout;
};

DashboardFrame BuildDashboard(const ObservationSnapshot& snapshot,
    const std::vector<DiagnosticFinding>& findings,
    const DashboardConstraints& constraints);

std::wstring PlainText(const StyledLine& line);

} // namespace powerscope
