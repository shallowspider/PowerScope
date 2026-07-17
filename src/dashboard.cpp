#include "dashboard.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <sstream>

namespace powerscope {
namespace {

StyledLine Line(std::wstring text, TextTone tone = TextTone::Default) {
    return {{std::move(text), tone}};
}

StyledLine Section(std::wstring title) {
    return {{L"── " + std::move(title) + L" ", TextTone::Muted}};
}

int DisplayCellWidth(wchar_t ch) {
    const unsigned int value = static_cast<unsigned int>(ch);
    if (value < 0x1100) return 1;
    if ((value >= 0x1100 && value <= 0x115F) || (value >= 0x2E80 && value <= 0xA4CF) ||
        (value >= 0xAC00 && value <= 0xD7A3) || (value >= 0xF900 && value <= 0xFAFF) ||
        (value >= 0xFE10 && value <= 0xFE6F) || (value >= 0xFF01 && value <= 0xFF60) ||
        (value >= 0xFFE0 && value <= 0xFFE6)) return 2;
    return 1;
}

int DisplayWidth(const std::wstring& text) {
    int width = 0;
    for (wchar_t ch : text) width += DisplayCellWidth(ch);
    return width;
}

std::wstring PadRightCells(std::wstring text, int width) {
    while (!text.empty() && DisplayWidth(text) > width - 1) text.pop_back();
    if (DisplayWidth(text) > width) text.clear();
    if (DisplayWidth(text) < width && !text.empty() && text.back() != L' ' &&
        DisplayWidth(text) == width - 1) text += L"…";
    const int padding = std::max(0, width - DisplayWidth(text));
    text.append(static_cast<std::size_t>(padding), L' ');
    return text;
}

std::wstring PadLeftCells(std::wstring text, int width) {
    const int padding = std::max(0, width - DisplayWidth(text));
    text.insert(0, static_cast<std::size_t>(padding), L' ');
    return text;
}

std::vector<StyledLine> WrapLine(const StyledLine& line, int width) {
    std::vector<StyledLine> lines(1);
    int used = 0;
    for (const auto& span : line) {
        for (wchar_t ch : span.text) {
            const int cells = DisplayCellWidth(ch);
            if (used + cells > width && used > 0) {
                lines.push_back({});
                used = 0;
            }
            if (lines.back().empty() || lines.back().back().tone != span.tone)
                lines.back().push_back({{}, span.tone});
            lines.back().back().text.push_back(ch);
            used += cells;
        }
    }
    return lines;
}

DashboardFrame WrapFrame(DashboardFrame frame, int width) {
    std::vector<StyledLine> wrapped;
    for (const auto& line : frame.lines) {
        auto pieces = WrapLine(line, std::max(20, width - 1));
        wrapped.insert(wrapped.end(), std::make_move_iterator(pieces.begin()), std::make_move_iterator(pieces.end()));
    }
    frame.lines = std::move(wrapped);
    return frame;
}

std::wstring FormatMaybe(double value, int precision = 1) {
    if (!std::isfinite(value)) return L"—";
    std::wostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::wstring FormatDuration(std::uint32_t seconds) {
    if (seconds == static_cast<std::uint32_t>(-1) || seconds == 0) return L"未知";
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    std::wstring text;
    if (hours > 0) text += std::to_wstring(hours) + L"小时";
    return text + std::to_wstring(minutes) + L"分钟";
}

std::wstring StatusReason(DataStatus status) {
    switch (status) {
    case DataStatus::Available: return L"可用";
    case DataStatus::Unmeasurable: return L"不可测量";
    case DataStatus::TemporarilyUnavailable: return L"当前不可用";
    case DataStatus::Disabled: return L"已禁用";
    case DataStatus::WarmingUp: return L"预热中";
    }
    return L"当前不可用";
}

std::wstring CompactStatus(DataStatus status) {
    switch (status) {
    case DataStatus::Available: return L"可用";
    case DataStatus::Unmeasurable: return L"不可测";
    case DataStatus::TemporarilyUnavailable: return L"暂缺";
    case DataStatus::Disabled: return L"禁用";
    case DataStatus::WarmingUp: return L"预热";
    }
    return L"暂缺";
}

TextTone ToneFor(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Hint: return TextTone::Hint;
    case DiagnosticSeverity::Attention: return TextTone::Attention;
    case DiagnosticSeverity::Severe: return TextTone::Severe;
    }
    return TextTone::Default;
}

std::wstring FindingText(const DiagnosticFinding& finding) {
    switch (finding.kind) {
    case DiagnosticKind::HighSystemDraw:
        if (finding.severity == DiagnosticSeverity::Severe)
            return L"严重：整机放电功耗超过 40 W，预计续航明显缩短。";
        if (finding.severity == DiagnosticSeverity::Attention)
            return L"注意：整机放电功耗超过 25 W，建议排查持续耗能来源。";
        return L"提示：当前整机功耗偏高，请结合 30/60 秒均值判断。";
    case DiagnosticKind::IdleGpuPower:
        return L"注意：独显利用率很低但仍有明显功耗，可能未进入休眠。";
    case DiagnosticKind::HighGpuPower:
        return L"提示：RTX 独显当前贡献了较明显的功耗。";
    case DiagnosticKind::HighCpuUtilization:
        return L"注意：CPU 总占用偏高，请查看活跃进程定位后台任务。";
    case DiagnosticKind::HighBrightness:
        return L"提示：内屏亮度较高，可能明显影响整机功耗。";
    case DiagnosticKind::HighRefreshRate:
        return L"提示：屏幕处于高刷新率，可切换至 60 Hz 对比续航。";
    case DiagnosticKind::HdrEnabled:
        return L"提示：HDR/高级颜色已开启，可能提高显示链路功耗。";
    case DiagnosticKind::HighProcessIo:
        return L"提示：进程磁盘 I/O 较高，可能造成系统持续活跃。";
    case DiagnosticKind::HighNetworkTraffic:
        return L"提示：网络吞吐量较高，可能存在下载、同步或更新。";
    }
    return L"提示：检测到可能影响续航的因素。";
}

std::wstring GpuText(const ObservationSnapshot& snapshot) {
    if (snapshot.gpu.status != DataStatus::Available) {
        return L"GPU  —（" + StatusReason(snapshot.gpu.status) + L"）";
    }
    const std::wstring power = std::isfinite(snapshot.gpu.power_w)
        ? FormatMaybe(snapshot.gpu.power_w) + L" W" : L"—（当前不可用）";
    const std::wstring utilization = snapshot.gpu.utilization >= 0
        ? std::to_wstring(snapshot.gpu.utilization) + L"%" : L"—（当前不可用）";
    const std::wstring temperature = snapshot.gpu.temperature_c >= 0
        ? std::to_wstring(snapshot.gpu.temperature_c) + L"°C" : L"—（当前不可用）";
    return L"GPU  " + snapshot.gpu.name + L"  " + power +
        L"  |  利用率 " + utilization + L"  |  温度 " + temperature;
}

std::wstring PowerText(const ObservationSnapshot& snapshot) {
    if (snapshot.battery.draw_status != DataStatus::Available ||
        !std::isfinite(snapshot.battery.system_draw_w)) {
        return L"整机功耗  —（" + StatusReason(snapshot.battery.draw_status) + L"）";
    }
    std::wstring text = L"整机功耗  " + FormatMaybe(snapshot.battery.system_draw_w) + L" W  |  5秒 " +
        FormatMaybe(snapshot.power_averages.seconds_5) + L" W  |  30秒 " +
        FormatMaybe(snapshot.power_averages.seconds_30) + L" W  |  60秒 " +
        FormatMaybe(snapshot.power_averages.seconds_60) + L" W";
    if (snapshot.power_averages.coverage_seconds < 60) {
        text += L"  |  均值预热 " + std::to_wstring(snapshot.power_averages.coverage_seconds) + L"/60 秒";
    }
    return text;
}

std::wstring BatteryText(const ObservationSnapshot& snapshot, bool include_duration) {
    std::wstring text = snapshot.battery.ac_online ? L"外接电源" : L"电池供电";
    text += L"  |  电量 " + (snapshot.battery.percent >= 0
        ? std::to_wstring(snapshot.battery.percent) + L"%" : L"—（当前不可用）");
    if (include_duration) {
        text += L"  |  预计 " + (snapshot.battery.estimate_status == DataStatus::Available
            ? FormatDuration(snapshot.battery.estimated_seconds)
            : L"—（" + StatusReason(snapshot.battery.estimate_status) + L"）");
    }
    return text + L"  |  节电 " + (snapshot.battery.saver ? L"开" : L"关");
}

std::wstring CpuText(const ObservationSnapshot& snapshot) {
    const std::wstring utilization = snapshot.cpu.utilization_status == DataStatus::Available
        ? FormatMaybe(snapshot.cpu.utilization) + L"%"
        : L"—（" + StatusReason(snapshot.cpu.utilization_status) + L"）";
    const std::wstring frequency = snapshot.cpu.frequency_status == DataStatus::Available
        ? FormatMaybe(snapshot.cpu.average_mhz / 1000.0, 2) + L" / " +
            FormatMaybe(snapshot.cpu.average_limit_mhz / 1000.0, 2) + L" GHz"
        : L"—（" + StatusReason(snapshot.cpu.frequency_status) + L"）";
    return L"CPU  " + utilization + L"  |  频率 " + frequency;
}

std::wstring MemoryText(const ObservationSnapshot& snapshot) {
    if (snapshot.memory.status != DataStatus::Available)
        return L"内存  —（" + StatusReason(snapshot.memory.status) + L"）";
    return L"内存  " + FormatMaybe(snapshot.memory.used_gib) + L" / " +
        FormatMaybe(snapshot.memory.total_gib) + L" GiB（" +
        FormatMaybe(snapshot.memory.used_percent, 0) + L"%）";
}

std::wstring DisplayText(const ObservationSnapshot& snapshot) {
    if (snapshot.display.status != DataStatus::Available)
        return L"显示  —（" + StatusReason(snapshot.display.status) + L"）";
    const std::wstring brightness = snapshot.display.brightness_status == DataStatus::Available
        ? std::to_wstring(snapshot.display.brightness_percent) + L"%"
        : L"—（" + StatusReason(snapshot.display.brightness_status) + L"）";
    const std::wstring hdr = snapshot.display.hdr_status == DataStatus::Available
        ? (snapshot.display.hdr_enabled ? L"开" : L"关")
        : L"—（" + StatusReason(snapshot.display.hdr_status) + L"）";
    return L"显示  " + std::to_wstring(snapshot.display.width) + L"×" +
        std::to_wstring(snapshot.display.height) + L" @ " + std::to_wstring(snapshot.display.refresh_hz) +
        L" Hz  |  亮度 " + brightness + L"  |  HDR " + hdr;
}

std::wstring NetworkText(const ObservationSnapshot& snapshot, bool include_scheme) {
    std::wstring text = snapshot.network.status == DataStatus::Available
        ? L"网络  ↓" + FormatMaybe(snapshot.network.receive_mib_s, 2) +
            L"  ↑" + FormatMaybe(snapshot.network.send_mib_s, 2) + L" MiB/s"
        : L"网络  —（" + StatusReason(snapshot.network.status) + L"）";
    if (include_scheme) text += L"  |  电源方案 " + snapshot.power_scheme;
    return text;
}

std::vector<StyledLine> DiagnosticLines(const std::vector<DiagnosticFinding>& findings,
    std::size_t limit) {
    std::vector<StyledLine> lines;
    if (findings.empty()) {
        lines.push_back(Line(L"  ✓ 当前未发现明显异常；优先观察 30 秒和 60 秒平均功耗。", TextTone::Key));
        return lines;
    }
    const std::size_t count = std::min(limit, findings.size());
    for (std::size_t i = 0; i < count; ++i) {
        lines.push_back(Line(L"  • " + FindingText(findings[i]), ToneFor(findings[i].severity)));
    }
    if (count < findings.size()) {
        lines.push_back(Line(L"  另有 " + std::to_wstring(findings.size() - count) + L" 条诊断结论未显示", TextTone::Muted));
    }
    return lines;
}

std::wstring ChooseText(std::initializer_list<std::wstring> choices, int width) {
    for (const auto& choice : choices) {
        if (DisplayWidth(choice) <= width) return choice;
    }
    return L"PowerScope";
}

std::wstring CompactFindingText(const DiagnosticFinding& finding) {
    switch (finding.kind) {
    case DiagnosticKind::HighSystemDraw: return L"整机功耗偏高，请观察持续均值。";
    case DiagnosticKind::IdleGpuPower: return L"独显低负载但仍有功耗，可能未休眠。";
    case DiagnosticKind::HighGpuPower: return L"独显当前功耗较高。";
    case DiagnosticKind::HighCpuUtilization: return L"CPU 占用偏高，请检查活跃进程。";
    case DiagnosticKind::HighBrightness: return L"屏幕亮度较高。";
    case DiagnosticKind::HighRefreshRate: return L"高刷新率可能影响续航。";
    case DiagnosticKind::HdrEnabled: return L"HDR 可能提高显示功耗。";
    case DiagnosticKind::HighProcessIo: return L"进程磁盘 I/O 较高。";
    case DiagnosticKind::HighNetworkTraffic: return L"网络吞吐量较高。";
    }
    return L"检测到可能影响续航的因素。";
}

DashboardFrame BuildCompactVertical(const ObservationSnapshot& snapshot,
    const std::vector<DiagnosticFinding>& findings, int width) {
    const int content_width = std::max(20, width - 1);
    DashboardFrame frame;
    frame.layout = DashboardLayout::Vertical;
    frame.lines.push_back(Line(ChooseText({L"PowerScope  " + snapshot.timestamp, L"PowerScope"}, content_width), TextTone::Key));
    frame.lines.push_back(Section(L"整机功耗"));
    if (snapshot.battery.draw_status == DataStatus::Available && std::isfinite(snapshot.battery.system_draw_w)) {
        frame.lines.push_back(Line(L"  当前 " + FormatMaybe(snapshot.battery.system_draw_w) + L" W", TextTone::Key));
        frame.lines.push_back(Line(ChooseText({L"  均值  5秒 " + FormatMaybe(snapshot.power_averages.seconds_5) +
            L" W  |  30秒 " + FormatMaybe(snapshot.power_averages.seconds_30) + L" W",
            L"  5/30秒 " + FormatMaybe(snapshot.power_averages.seconds_5, 0) + L"/" +
                FormatMaybe(snapshot.power_averages.seconds_30, 0) + L"W"}, content_width), TextTone::Key));
        frame.lines.push_back(Line(ChooseText({L"  60秒 " + FormatMaybe(snapshot.power_averages.seconds_60) +
                L" W  |  预热 " + std::to_wstring(snapshot.power_averages.coverage_seconds) + L"/60 秒",
            L"  60秒 " + FormatMaybe(snapshot.power_averages.seconds_60, 0) + L"W"}, content_width), TextTone::Muted));
    } else {
        frame.lines.push_back(Line(L"  —（" + StatusReason(snapshot.battery.draw_status) + L"）", TextTone::Key));
        frame.lines.push_back(Line(L"  5/30秒：预热中", TextTone::Muted));
        frame.lines.push_back(Line(L"  60秒：预热中", TextTone::Muted));
    }
    frame.lines.push_back(Section(content_width >= 32 ? L"电池与电源 / 核心资源" : L"电池 / 资源"));
    const std::wstring battery_percent = snapshot.battery.percent >= 0
        ? std::to_wstring(snapshot.battery.percent) + L"%" : L"—";
    frame.lines.push_back(Line(ChooseText({L"  " + BatteryText(snapshot, false),
        L"  电池 " + battery_percent + L"  " + (snapshot.battery.ac_online ? L"外接" : L"放电")}, content_width)));
    const std::wstring cpu_short = snapshot.cpu.utilization_status == DataStatus::Available
        ? FormatMaybe(snapshot.cpu.utilization, 0) + L"%" : CompactStatus(snapshot.cpu.utilization_status);
    const std::wstring memory_short = snapshot.memory.status == DataStatus::Available
        ? FormatMaybe(snapshot.memory.used_percent, 0) + L"%" : CompactStatus(snapshot.memory.status);
    frame.lines.push_back(Line(content_width >= 24
        ? L"  CPU " + cpu_short + L"  |  内存 " + memory_short
        : L"CPU" + cpu_short + L" RAM" + memory_short));
    const std::wstring gpu_status = snapshot.gpu.status != DataStatus::Available
        ? StatusReason(snapshot.gpu.status)
        : (std::isfinite(snapshot.gpu.power_w) ? FormatMaybe(snapshot.gpu.power_w) + L" W" : L"当前不可用");
    const std::wstring gpu_util = snapshot.gpu.utilization >= 0
        ? std::to_wstring(snapshot.gpu.utilization) + L"%" : L"当前不可用";
    frame.lines.push_back(Line(ChooseText({L"  GPU " + gpu_status + L"  |  利用率 " + gpu_util,
        L"  GPU " + gpu_status}, content_width)));
    frame.lines.push_back(Section(content_width >= 28 ? L"显示与网络" : L"显示 / 网络"));
    const std::wstring display_short = snapshot.display.status == DataStatus::Available
        ? std::to_wstring(snapshot.display.width) + L"×" + std::to_wstring(snapshot.display.height) +
            L"@" + std::to_wstring(snapshot.display.refresh_hz)
        : StatusReason(snapshot.display.status);
    frame.lines.push_back(Line(ChooseText({L"  显示 " + display_short,
        L"  显示 " + StatusReason(snapshot.display.status)}, content_width)));
    const std::wstring network_short = snapshot.network.status == DataStatus::Available
        ? L"↓" + FormatMaybe(snapshot.network.receive_mib_s, 1) + L" ↑" + FormatMaybe(snapshot.network.send_mib_s, 1)
        : StatusReason(snapshot.network.status);
    frame.lines.push_back(Line(ChooseText({L"  网络 " + network_short, L"  网络"}, content_width)));
    frame.lines.push_back(Section(content_width >= 24 ? L"活跃进程" : L"进程"));
    if (snapshot.processes.top.empty()) {
        frame.lines.push_back(Line(L"  暂无明显活动", TextTone::Muted));
    } else {
        const auto& process = snapshot.processes.top.front();
        const std::wstring hidden = snapshot.processes.top.size() > 1
            ? L"  +" + std::to_wstring(snapshot.processes.top.size() - 1) + L" 未显示" : L"";
        frame.lines.push_back(Line(ChooseText({L"  " + process.name + L"  PID " + std::to_wstring(process.pid) + hidden,
            L"  PID " + std::to_wstring(process.pid) + L"  CPU " + FormatMaybe(process.cpu_percent) + L"%" + hidden,
            L"  PID " + std::to_wstring(process.pid) + hidden}, content_width)));
    }
    frame.lines.push_back(Section(content_width >= 24 ? L"自动诊断" : L"诊断"));
    if (findings.empty()) {
        frame.lines.push_back(Line(ChooseText({L"  ✓ 当前未发现明显异常。", L"  ✓ 无明显异常"}, content_width), TextTone::Key));
    } else {
        const std::wstring hidden = findings.size() > 1
            ? L"  +" + std::to_wstring(findings.size() - 1) + L" 条" : L"";
        frame.lines.push_back(Line(ChooseText({L"  • " + CompactFindingText(findings.front()) + hidden,
            L"  • " + CompactFindingText(findings.front()), L"  • 诊断结论" + hidden}, content_width),
            ToneFor(findings.front().severity)));
    }
    return frame;
}

DashboardFrame BuildVertical(const ObservationSnapshot& snapshot,
    const std::vector<DiagnosticFinding>& findings, const DashboardConstraints& constraints) {
    DashboardFrame frame;
    frame.layout = DashboardLayout::Vertical;
    frame.lines.push_back({{L"PowerScope", TextTone::Key},
        {L"  " + snapshot.timestamp + L"  |  刷新 " + std::to_wstring(snapshot.options.interval_ms) + L" ms", TextTone::Muted}});
    frame.lines.push_back(Section(L"整机功耗"));
    frame.lines.push_back(Line(L"  " + PowerText(snapshot), TextTone::Key));
    frame.lines.push_back(Section(L"电池与电源"));
    frame.lines.push_back(Line(L"  " + BatteryText(snapshot, true)));
    frame.lines.push_back(Section(L"核心资源"));
    frame.lines.push_back(Line(L"  " + CpuText(snapshot)));
    frame.lines.push_back(Line(L"  " + MemoryText(snapshot)));
    frame.lines.push_back(Line(L"  " + GpuText(snapshot)));
    frame.lines.push_back(Section(L"显示与网络"));
    frame.lines.push_back(Line(L"  " + DisplayText(snapshot)));
    frame.lines.push_back(Line(L"  " + NetworkText(snapshot, true)));
    frame.lines.push_back(Section(L"活跃进程"));

    const std::size_t diagnosis_limit = std::max<std::size_t>(1, std::min<std::size_t>(4, findings.size()));
    const std::size_t diagnosis_rows = diagnosis_limit + (findings.size() > diagnosis_limit ? 1 : 0);
    const std::size_t minimum_process_rows = snapshot.processes.top.empty()
        ? 1 : 2 + (snapshot.processes.top.size() > 1 ? 1 : 0);
    if (frame.lines.size() + minimum_process_rows + 1 + diagnosis_rows >
        static_cast<std::size_t>(std::max(16, constraints.height))) {
        return BuildCompactVertical(snapshot, findings, constraints.width);
    }
    const int fixed_rows = static_cast<int>(frame.lines.size()) + 1 + static_cast<int>(diagnosis_rows);
    std::size_t shown = snapshot.processes.top.empty() ? 0 : 1;
    while (shown < snapshot.processes.top.size() && shown < 6) {
        const bool will_hide = shown + 1 < snapshot.processes.top.size();
        const int projected = fixed_rows + static_cast<int>((shown + 1) * 2) + (will_hide ? 1 : 0);
        if (projected > std::max(16, constraints.height)) break;
        ++shown;
    }

    if (snapshot.processes.top.empty()) {
        frame.lines.push_back(Line(L"  暂无明显活动", TextTone::Muted));
    } else {
        for (std::size_t i = 0; i < shown; ++i) {
            const auto& process = snapshot.processes.top[i];
            frame.lines.push_back(Line(L"  " + process.name + L"  ·  PID " + std::to_wstring(process.pid), TextTone::Default));
            frame.lines.push_back(Line(L"    CPU " + FormatMaybe(process.cpu_percent) + L"%  |  读取 " +
                FormatMaybe(process.read_mib_s, 2) + L"  写入 " + FormatMaybe(process.write_mib_s, 2) + L" MiB/s", TextTone::Muted));
        }
        if (shown < snapshot.processes.top.size()) {
            frame.lines.push_back(Line(L"  另有 " + std::to_wstring(snapshot.processes.top.size() - shown) + L" 个进程未显示", TextTone::Muted));
        }
    }
    frame.lines.push_back(Section(L"自动诊断"));
    auto diagnosis = DiagnosticLines(findings, diagnosis_limit);
    frame.lines.insert(frame.lines.end(), diagnosis.begin(), diagnosis.end());
    return frame;
}

DashboardFrame BuildWide(const ObservationSnapshot& snapshot,
    const std::vector<DiagnosticFinding>& findings, const DashboardConstraints& constraints) {
    DashboardFrame frame;
    frame.layout = DashboardLayout::Wide;
    frame.lines.push_back({{L"PowerScope", TextTone::Key},
        {L"  |  " + snapshot.timestamp + L"  |  刷新 " + std::to_wstring(snapshot.options.interval_ms) + L" ms  |  Ctrl+C 退出", TextTone::Muted}});
    frame.lines.push_back(Line(L"─────────────────────────────────────────────────────────────────────────────────────────────────────────────", TextTone::Muted));
    frame.lines.push_back(Line(PowerText(snapshot), TextTone::Key));
    frame.lines.push_back(Line(L"电池  " + BatteryText(snapshot, true)));
    frame.lines.push_back(Line(CpuText(snapshot) + L"  |  " + MemoryText(snapshot)));
    frame.lines.push_back(Line(GpuText(snapshot)));
    frame.lines.push_back(Line(DisplayText(snapshot) + L"  |  方案 " + snapshot.power_scheme));
    frame.lines.push_back(Line(L"活动  网络 ↓" + FormatMaybe(snapshot.network.receive_mib_s, 2) + L" ↑" +
        FormatMaybe(snapshot.network.send_mib_s, 2) + L" MiB/s  |  进程 I/O 读 " +
        FormatMaybe(snapshot.processes.total_read_mib_s, 2) + L" 写 " + FormatMaybe(snapshot.processes.total_write_mib_s, 2) + L" MiB/s"));
    frame.lines.push_back(Section(L"活跃进程"));
    frame.lines.push_back(Line(L"  进程                              PID     CPU%      读 MiB/s      写 MiB/s", TextTone::Muted));
    const std::size_t diagnosis_limit = std::max<std::size_t>(1, std::min<std::size_t>(4, findings.size()));
    const std::size_t diagnosis_rows = diagnosis_limit + (findings.size() > diagnosis_limit ? 1 : 0);
    std::size_t process_limit = std::min<std::size_t>(6, std::max<std::size_t>(1, snapshot.processes.top.size()));
    while (process_limit > 1) {
        const std::size_t hidden_row = snapshot.processes.top.size() > process_limit ? 1 : 0;
        if (frame.lines.size() + process_limit + hidden_row + 1 + diagnosis_rows <=
            static_cast<std::size_t>(std::max(16, constraints.height))) break;
        --process_limit;
    }
    for (std::size_t i = 0; i < std::min(process_limit, snapshot.processes.top.size()); ++i) {
        const auto& process = snapshot.processes.top[i];
        const std::wstring row = L"  " + PadRightCells(process.name, 30) +
            PadLeftCells(std::to_wstring(process.pid), 8) +
            PadLeftCells(FormatMaybe(process.cpu_percent), 9) +
            PadLeftCells(FormatMaybe(process.read_mib_s, 2), 14) +
            PadLeftCells(FormatMaybe(process.write_mib_s, 2), 14);
        frame.lines.push_back(Line(row));
    }
    if (snapshot.processes.top.empty()) frame.lines.push_back(Line(L"  暂无明显活动", TextTone::Muted));
    else if (process_limit < snapshot.processes.top.size())
        frame.lines.push_back(Line(L"  另有 " + std::to_wstring(snapshot.processes.top.size() - process_limit) + L" 个进程未显示", TextTone::Muted));
    frame.lines.push_back(Section(L"自动诊断"));
    auto diagnosis = DiagnosticLines(findings, diagnosis_limit);
    frame.lines.insert(frame.lines.end(), diagnosis.begin(), diagnosis.end());
    return frame;
}

} // namespace

DashboardFrame BuildDashboard(const ObservationSnapshot& snapshot,
    const std::vector<DiagnosticFinding>& findings,
    const DashboardConstraints& constraints) {
    constexpr int kWideThreshold = 110;
    constexpr int kHysteresis = 4;
    const bool was_wide = constraints.previous_layout == DashboardLayout::Wide;
    const bool use_wide = constraints.width >= kWideThreshold ||
        (was_wide && constraints.width >= kWideThreshold - kHysteresis);
    if (constraints.height < 24)
        return BuildCompactVertical(snapshot, findings, constraints.width);
    if (use_wide) {
        auto frame = BuildWide(snapshot, findings, constraints);
        if (frame.lines.size() <= static_cast<std::size_t>(constraints.height)) return frame;
        return BuildCompactVertical(snapshot, findings, constraints.width);
    }

    DashboardConstraints adjusted = constraints;
    while (adjusted.height >= 16) {
        auto frame = WrapFrame(BuildVertical(snapshot, findings, adjusted), constraints.width);
        if (frame.lines.size() <= static_cast<std::size_t>(constraints.height) || adjusted.height == 16)
            return frame;
        --adjusted.height;
    }
    return BuildCompactVertical(snapshot, findings, constraints.width);
}

std::wstring PlainText(const StyledLine& line) {
    std::wstring text;
    for (const auto& span : line) text += span.text;
    return text;
}

} // namespace powerscope
