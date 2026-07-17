#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "dashboard.h"
#include "diagnostics.h"
#include "monitoring_session.h"
#include "terminal_renderer.h"
#include "windows_probes.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <optional>
#include <string>

namespace {

std::atomic_bool g_stop{false};

BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

void PrintHelp() {
    std::wcout
        << L"PowerScope - Windows 11 笔记本实时耗能诊断器\n\n"
        << L"用法：PowerScope.exe [选项]\n\n"
        << L"  --interval-ms N       主采样间隔，默认 1000，最低 500\n"
        << L"  --gpu-interval-ms N   NVML 采样间隔，默认 5000，最低 1000\n"
        << L"  --no-gpu              禁用 NVML，避免 GPU 查询影响独显休眠\n"
        << L"  --help                 显示帮助\n\n"
        << L"说明：电池供电时的整机功耗来自电池控制器；插电时 Windows 没有通用的整机输入功率接口。\n";
}

std::optional<powerscope::Options> ParseOptions(int argc, wchar_t** argv) {
    powerscope::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto require_value = [&](const wchar_t* name) -> std::optional<std::wstring> {
            if (i + 1 >= argc) {
                std::wcerr << L"缺少参数值：" << name << L'\n';
                return std::nullopt;
            }
            return std::wstring(argv[++i]);
        };

        if (arg == L"--help" || arg == L"-h") {
            PrintHelp();
            return std::nullopt;
        }
        if (arg == L"--interval-ms") {
            const auto value = require_value(L"--interval-ms");
            if (!value) return std::nullopt;
            try { options.interval_ms = std::max<std::uint32_t>(500, std::stoul(*value)); }
            catch (...) { std::wcerr << L"无效采样间隔。\n"; return std::nullopt; }
            continue;
        }
        if (arg == L"--gpu-interval-ms") {
            const auto value = require_value(L"--gpu-interval-ms");
            if (!value) return std::nullopt;
            try { options.gpu_interval_ms = std::max<std::uint32_t>(1000, std::stoul(*value)); }
            catch (...) { std::wcerr << L"无效 GPU 采样间隔。\n"; return std::nullopt; }
            continue;
        }
        if (arg == L"--no-gpu") {
            options.gpu_enabled = false;
            continue;
        }
        std::wcerr << L"未知选项：" << arg << L'\n';
        PrintHelp();
        return std::nullopt;
    }
    return options;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    powerscope::InitializeConsoleOutput();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    const auto options = ParseOptions(argc, argv);
    if (!options) return argc > 1 ? 0 : 1;

    std::wcout << L"PowerScope 正在初始化监测器，请稍候……" << std::flush;
    powerscope::WindowsObservationSource source;
    powerscope::WindowsSessionRuntime runtime(g_stop);
    powerscope::MonitoringSession session(*options, source, runtime);
    powerscope::TerminalRenderer renderer;
    std::optional<powerscope::DashboardLayout> previous_layout;

    session.Run([&](const powerscope::ObservationSnapshot& snapshot) {
        const auto findings = powerscope::EvaluateDiagnostics(snapshot);
        const auto terminal = renderer.Size();
        auto frame = powerscope::BuildDashboard(snapshot, findings,
            {terminal.width, terminal.height, previous_layout});
        previous_layout = frame.layout;
        renderer.Render(frame);
    });

    std::wcout << L"\n已停止监测。\n";
    return 0;
}
