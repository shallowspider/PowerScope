#ifndef WINVER
#define WINVER 0x0A00
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Winsock 2 must be included before windows.h.  The modern IP Helper
// declarations used by GetIfTable2 depend on these networking types.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <powrprof.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <oleauto.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Wbemuuid.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kBytesPerMiB = 1024.0 * 1024.0;
std::atomic_bool g_stop{false};

BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

uint64_t FileTimeToU64(const FILETIME& value) {
    ULARGE_INTEGER u{};
    u.LowPart = value.dwLowDateTime;
    u.HighPart = value.dwHighDateTime;
    return u.QuadPart;
}

std::wstring NowLocalText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, std::size(buffer), L"%04u-%02u-%02u %02u:%02u:%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring FormatDuration(DWORD seconds) {
    if (seconds == static_cast<DWORD>(-1) || seconds == 0) return L"未知";
    const DWORD hours = seconds / 3600;
    const DWORD minutes = (seconds % 3600) / 60;
    std::wostringstream out;
    if (hours > 0) out << hours << L"小时";
    out << minutes << L"分钟";
    return out.str();
}

std::wstring FormatMaybe(double value, int precision = 1) {
    if (!std::isfinite(value)) return L"N/A";
    std::wostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

bool g_virtual_terminal_enabled = false;

void InitializeConsoleOutput() {
    // MSVC wide streams are not reliably Unicode-capable on every Windows
    // terminal unless stdout/stderr are explicitly switched to UTF-16 mode.
    // Without this, the first Chinese character can set failbit on std::wcout,
    // leaving the terminal blank even though background sampling continues normally.
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || output == nullptr) return;

    DWORD mode = 0;
    if (GetConsoleMode(output, &mode) &&
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        g_virtual_terminal_enabled = true;
    }
}

void SetConsoleCursorVisible(bool visible) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || output == nullptr) return;
    CONSOLE_CURSOR_INFO cursor{};
    if (!GetConsoleCursorInfo(output, &cursor)) return;
    cursor.bVisible = visible ? TRUE : FALSE;
    SetConsoleCursorInfo(output, &cursor);
}

int DisplayCellWidth(wchar_t ch) {
    const unsigned int c = static_cast<unsigned int>(ch);
    if (c == 0) return 0;
    if (c < 0x1100) return 1;
    // Common East Asian wide/full-width ranges used by this dashboard.
    if ((c >= 0x1100 && c <= 0x115F) ||
        (c >= 0x2E80 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) ||
        (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE10 && c <= 0xFE19) ||
        (c >= 0xFE30 && c <= 0xFE6F) ||
        (c >= 0xFF01 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6)) {
        return 2;
    }
    return 1;
}

size_t DisplayWidth(const std::wstring& text) {
    size_t width = 0;
    for (wchar_t ch : text) width += static_cast<size_t>(DisplayCellWidth(ch));
    return width;
}

std::wstring TruncateToWidth(const std::wstring& text, size_t width) {
    if (DisplayWidth(text) <= width) return text;
    if (width == 0) return {};
    const std::wstring ellipsis = L"…";
    const size_t target = width > 1 ? width - 1 : 0;
    std::wstring result;
    size_t used = 0;
    for (wchar_t ch : text) {
        const size_t cell = static_cast<size_t>(DisplayCellWidth(ch));
        if (used + cell > target) break;
        result.push_back(ch);
        used += cell;
    }
    if (width >= 1) result += ellipsis;
    return result;
}

std::wstring PadRightCells(std::wstring text, size_t width) {
    text = TruncateToWidth(text, width);
    const size_t used = DisplayWidth(text);
    if (used < width) text.append(width - used, L' ');
    return text;
}

std::wstring PadLeftCells(std::wstring text, size_t width) {
    text = TruncateToWidth(text, width);
    const size_t used = DisplayWidth(text);
    if (used < width) text.insert(0, width - used, L' ');
    return text;
}

class TerminalRenderer {
public:
    TerminalRenderer() {
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        valid_ = output_ != INVALID_HANDLE_VALUE && output_ != nullptr;
        if (!valid_) return;
        SetConsoleCursorVisible(false);
        WriteRaw(g_virtual_terminal_enabled ? L"\x1b[2J\x1b[H\x1b[?25l" : L"");
        if (!g_virtual_terminal_enabled) {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (GetConsoleScreenBufferInfo(output_, &info)) {
                const DWORD cells = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
                DWORD written = 0;
                FillConsoleOutputCharacterW(output_, L' ', cells, COORD{0, 0}, &written);
                FillConsoleOutputAttribute(output_, info.wAttributes, cells, COORD{0, 0}, &written);
                SetConsoleCursorPosition(output_, COORD{0, 0});
            }
        }
    }

    ~TerminalRenderer() {
        if (!valid_) return;
        if (g_virtual_terminal_enabled) WriteRaw(L"\x1b[?25h");
        SetConsoleCursorVisible(true);
    }

    void Render(const std::vector<std::wstring>& lines) {
        if (!valid_) return;
        const int width = ConsoleWidth();
        const size_t content_width = static_cast<size_t>(std::max(20, width - 1));
        const size_t rows = std::max(previous_rows_, lines.size());

        std::wstring frame;
        frame.reserve(rows * (content_width + 1) + 16);
        if (g_virtual_terminal_enabled) frame += L"\x1b[H";
        else SetConsoleCursorPosition(output_, COORD{0, 0});

        for (size_t i = 0; i < rows; ++i) {
            const std::wstring line = i < lines.size() ? lines[i] : L"";
            frame += PadRightCells(line, content_width);
            if (i + 1 < rows) frame += L"\r\n";
        }
        if (g_virtual_terminal_enabled) frame += L"\x1b[J";
        WriteRaw(frame);
        previous_rows_ = lines.size();
    }

private:
    int ConsoleWidth() const {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (valid_ && GetConsoleScreenBufferInfo(output_, &info)) {
            return std::max<int>(20, info.srWindow.Right - info.srWindow.Left + 1);
        }
        return 100;
    }

    void WriteRaw(const std::wstring& text) const {
        if (!valid_ || text.empty()) return;
        DWORD written = 0;
        WriteConsoleW(output_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }

    HANDLE output_ = INVALID_HANDLE_VALUE;
    bool valid_ = false;
    size_t previous_rows_ = 0;
};

struct Options {
    DWORD interval_ms = 1000;
    DWORD gpu_interval_ms = 5000;
    bool gpu_enabled = true;
};

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

std::optional<Options> ParseOptions(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto require_value = [&](const wchar_t* name) -> std::optional<std::wstring> {
            if (i + 1 >= argc) {
                std::wcerr << L"缺少参数值：" << name << L"\n";
                return std::nullopt;
            }
            return std::wstring(argv[++i]);
        };

        if (arg == L"--help" || arg == L"-h") {
            PrintHelp();
            return std::nullopt;
        } else if (arg == L"--interval-ms") {
            auto v = require_value(L"--interval-ms");
            if (!v) return std::nullopt;
            try { options.interval_ms = std::max<DWORD>(500, std::stoul(*v)); }
            catch (...) { std::wcerr << L"无效采样间隔。\n"; return std::nullopt; }
        } else if (arg == L"--gpu-interval-ms") {
            auto v = require_value(L"--gpu-interval-ms");
            if (!v) return std::nullopt;
            try { options.gpu_interval_ms = std::max<DWORD>(1000, std::stoul(*v)); }
            catch (...) { std::wcerr << L"无效 GPU 采样间隔。\n"; return std::nullopt; }
        } else if (arg == L"--no-gpu") {
            options.gpu_enabled = false;
        } else {
            std::wcerr << L"未知选项：" << arg << L"\n";
            PrintHelp();
            return std::nullopt;
        }
    }
    return options;
}

struct BatteryReading {
    bool available = false;
    bool ac_online = false;
    bool charging = false;
    bool discharging = false;
    bool saver = false;
    int percent = -1;
    double signed_rate_w = std::numeric_limits<double>::quiet_NaN();
    double system_draw_w = std::numeric_limits<double>::quiet_NaN();
    double remaining_wh = std::numeric_limits<double>::quiet_NaN();
    double max_wh = std::numeric_limits<double>::quiet_NaN();
    DWORD estimated_seconds = static_cast<DWORD>(-1);
};

BatteryReading ReadBattery() {
    BatteryReading result;
    SYSTEM_POWER_STATUS basic{};
    if (GetSystemPowerStatus(&basic)) {
        result.ac_online = basic.ACLineStatus == 1;
        result.saver = basic.SystemStatusFlag != 0;
        if (basic.BatteryLifePercent != 255) result.percent = basic.BatteryLifePercent;
        result.estimated_seconds = basic.BatteryLifeTime;
    }

    SYSTEM_BATTERY_STATE state{};
    const auto status = CallNtPowerInformation(
        SystemBatteryState, nullptr, 0, &state, sizeof(state));
    if (status == 0 && state.BatteryPresent) {
        result.available = true;
        result.ac_online = state.AcOnLine != FALSE;
        result.charging = state.Charging != FALSE;
        result.discharging = state.Discharging != FALSE;
        const LONG signed_rate_mw = static_cast<LONG>(state.Rate);
        result.signed_rate_w = static_cast<double>(signed_rate_mw) / 1000.0;
        if ((result.discharging || !result.ac_online) && signed_rate_mw < 0) {
            result.system_draw_w = -result.signed_rate_w;
        }
        if (state.RemainingCapacity != 0) {
            result.remaining_wh = static_cast<double>(state.RemainingCapacity) / 1000.0;
        }
        if (state.MaxCapacity != 0) {
            result.max_wh = static_cast<double>(state.MaxCapacity) / 1000.0;
        }
        if (state.EstimatedTime != static_cast<ULONG>(-1) && state.EstimatedTime != 0) {
            result.estimated_seconds = state.EstimatedTime;
        }
    }
    return result;
}


struct ProcessorPowerInformationLocal {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

struct CpuReading {
    double utilization = std::numeric_limits<double>::quiet_NaN();
    double average_mhz = std::numeric_limits<double>::quiet_NaN();
    double average_max_mhz = std::numeric_limits<double>::quiet_NaN();
    double average_limit_mhz = std::numeric_limits<double>::quiet_NaN();
};

class CpuSampler {
public:
    CpuSampler() {
        logical_processors_ = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        SampleTimes();
    }

    CpuReading Sample() {
        CpuReading result;
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const uint64_t idle_now = FileTimeToU64(idle);
            const uint64_t kernel_now = FileTimeToU64(kernel);
            const uint64_t user_now = FileTimeToU64(user);
            if (has_times_) {
                const uint64_t idle_delta = idle_now - idle_;
                const uint64_t kernel_delta = kernel_now - kernel_;
                const uint64_t user_delta = user_now - user_;
                const uint64_t total = kernel_delta + user_delta;
                if (total > 0 && total >= idle_delta) {
                    result.utilization = 100.0 * static_cast<double>(total - idle_delta)
                        / static_cast<double>(total);
                }
            }
            idle_ = idle_now;
            kernel_ = kernel_now;
            user_ = user_now;
            has_times_ = true;
        }

        std::vector<ProcessorPowerInformationLocal> info(logical_processors_);
        const auto status = CallNtPowerInformation(
            ProcessorInformation, nullptr, 0, info.data(),
            static_cast<ULONG>(info.size() * sizeof(ProcessorPowerInformationLocal)));
        if (status == 0 && !info.empty()) {
            double current = 0.0, maximum = 0.0, limit = 0.0;
            for (const auto& cpu : info) {
                current += cpu.CurrentMhz;
                maximum += cpu.MaxMhz;
                limit += cpu.MhzLimit;
            }
            result.average_mhz = current / info.size();
            result.average_max_mhz = maximum / info.size();
            result.average_limit_mhz = limit / info.size();
        }
        return result;
    }

    DWORD LogicalProcessors() const { return logical_processors_; }

private:
    void SampleTimes() {
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            idle_ = FileTimeToU64(idle);
            kernel_ = FileTimeToU64(kernel);
            user_ = FileTimeToU64(user);
            has_times_ = true;
        }
    }

    DWORD logical_processors_ = 1;
    bool has_times_ = false;
    uint64_t idle_ = 0, kernel_ = 0, user_ = 0;
};

struct ProcessReading {
    DWORD pid = 0;
    std::wstring name;
    double cpu_percent = 0.0;
    double read_mib_s = 0.0;
    double write_mib_s = 0.0;
};

struct ProcessSampleResult {
    std::vector<ProcessReading> top;
    double total_read_mib_s = 0.0;
    double total_write_mib_s = 0.0;
};

class ProcessSampler {
public:
    explicit ProcessSampler(DWORD logical_processors) : logical_processors_(std::max<DWORD>(1, logical_processors)) {}

    ProcessSampleResult Sample(double elapsed_seconds) {
        ProcessSampleResult output;
        if (elapsed_seconds <= 0.0) return output;

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return output;

        std::unordered_map<DWORD, Prev> next;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                const DWORD pid = entry.th32ProcessID;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!process) continue;

                FILETIME creation{}, exit{}, kernel{}, user{};
                IO_COUNTERS io{};
                const bool time_ok = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
                const bool io_ok = GetProcessIoCounters(process, &io) != FALSE;
                CloseHandle(process);
                if (!time_ok) continue;

                Prev current{};
                current.creation = FileTimeToU64(creation);
                current.cpu = FileTimeToU64(kernel) + FileTimeToU64(user);
                current.read = io_ok ? io.ReadTransferCount : 0;
                current.write = io_ok ? io.WriteTransferCount : 0;
                current.name = entry.szExeFile;
                next.emplace(pid, current);

                const auto old = previous_.find(pid);
                if (old == previous_.end() || old->second.creation != current.creation) continue;

                ProcessReading reading;
                reading.pid = pid;
                reading.name = current.name;
                const uint64_t cpu_delta = current.cpu >= old->second.cpu ? current.cpu - old->second.cpu : 0;
                reading.cpu_percent = (static_cast<double>(cpu_delta) / 10'000'000.0)
                    / elapsed_seconds / logical_processors_ * 100.0;
                if (io_ok) {
                    const uint64_t read_delta = current.read >= old->second.read ? current.read - old->second.read : 0;
                    const uint64_t write_delta = current.write >= old->second.write ? current.write - old->second.write : 0;
                    reading.read_mib_s = static_cast<double>(read_delta) / kBytesPerMiB / elapsed_seconds;
                    reading.write_mib_s = static_cast<double>(write_delta) / kBytesPerMiB / elapsed_seconds;
                    output.total_read_mib_s += reading.read_mib_s;
                    output.total_write_mib_s += reading.write_mib_s;
                }

                if (reading.cpu_percent >= 0.05 || reading.read_mib_s + reading.write_mib_s >= 0.05) {
                    output.top.push_back(std::move(reading));
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        previous_ = std::move(next);

        std::sort(output.top.begin(), output.top.end(), [](const auto& a, const auto& b) {
            const double score_a = a.cpu_percent + std::min(20.0, a.read_mib_s + a.write_mib_s);
            const double score_b = b.cpu_percent + std::min(20.0, b.read_mib_s + b.write_mib_s);
            return score_a > score_b;
        });
        if (output.top.size() > 6) output.top.resize(6);
        return output;
    }

private:
    struct Prev {
        uint64_t creation = 0;
        uint64_t cpu = 0;
        uint64_t read = 0;
        uint64_t write = 0;
        std::wstring name;
    };

    DWORD logical_processors_ = 1;
    std::unordered_map<DWORD, Prev> previous_;
};

struct NetworkReading {
    double receive_mib_s = 0.0;
    double send_mib_s = 0.0;
};

class NetworkSampler {
public:
    NetworkReading Sample(double elapsed_seconds) {
        NetworkReading result;
        if (elapsed_seconds <= 0.0) return result;
        const auto totals = ReadTotals();
        if (has_previous_) {
            const uint64_t in_delta = totals.first >= previous_in_ ? totals.first - previous_in_ : 0;
            const uint64_t out_delta = totals.second >= previous_out_ ? totals.second - previous_out_ : 0;
            result.receive_mib_s = static_cast<double>(in_delta)
                / kBytesPerMiB / elapsed_seconds;
            result.send_mib_s = static_cast<double>(out_delta)
                / kBytesPerMiB / elapsed_seconds;
        }
        previous_in_ = totals.first;
        previous_out_ = totals.second;
        has_previous_ = true;
        return result;
    }

private:
    static std::pair<uint64_t, uint64_t> ReadTotals() {
        PMIB_IF_TABLE2 table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR || table == nullptr) return {};

        uint64_t in_hardware = 0, out_hardware = 0;
        uint64_t in_fallback = 0, out_fallback = 0;
        bool found_hardware = false;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            in_fallback += row.InOctets;
            out_fallback += row.OutOctets;
            if (row.InterfaceAndOperStatusFlags.HardwareInterface) {
                found_hardware = true;
                in_hardware += row.InOctets;
                out_hardware += row.OutOctets;
            }
        }
        FreeMibTable(table);
        return found_hardware ? std::make_pair(in_hardware, out_hardware)
                              : std::make_pair(in_fallback, out_fallback);
    }

    bool has_previous_ = false;
    uint64_t previous_in_ = 0;
    uint64_t previous_out_ = 0;
};

struct MemoryReading {
    double used_percent = std::numeric_limits<double>::quiet_NaN();
    double used_gib = std::numeric_limits<double>::quiet_NaN();
    double total_gib = std::numeric_limits<double>::quiet_NaN();
};

MemoryReading ReadMemory() {
    MemoryReading result;
    MEMORYSTATUSEX state{};
    state.dwLength = sizeof(state);
    if (GlobalMemoryStatusEx(&state)) {
        result.used_percent = state.dwMemoryLoad;
        result.total_gib = static_cast<double>(state.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        result.used_gib = static_cast<double>(state.ullTotalPhys - state.ullAvailPhys)
            / (1024.0 * 1024.0 * 1024.0);
    }
    return result;
}

struct DisplayReading {
    int width = 0;
    int height = 0;
    int refresh_hz = 0;
    int active_displays = 0;
    bool hdr_supported = false;
    bool hdr_enabled = false;
    int brightness_percent = -1;
};

class BrightnessReader {
public:
    BrightnessReader() { Initialize(); }
    ~BrightnessReader() {
        if (services_) services_->Release();
        if (com_initialized_) CoUninitialize();
    }

    int Read() const {
        if (!services_) return -1;
        BSTR language = SysAllocString(L"WQL");
        BSTR query = SysAllocString(
            L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
        if (!language || !query) {
            if (language) SysFreeString(language);
            if (query) SysFreeString(query);
            return -1;
        }

        IEnumWbemClassObject* enumerator = nullptr;
        const HRESULT hr = services_->ExecQuery(language, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &enumerator);
        SysFreeString(language);
        SysFreeString(query);
        if (FAILED(hr) || !enumerator) return -1;

        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        int result = -1;
        if (enumerator->Next(1000, 1, &object, &returned) == WBEM_S_NO_ERROR && returned == 1) {
            VARIANT value{};
            VariantInit(&value);
            if (SUCCEEDED(object->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr))) {
                if (value.vt == VT_UI1) result = value.bVal;
                else if (value.vt == VT_I4) result = value.lVal;
                else if (value.vt == VT_UI4) result = static_cast<int>(value.ulVal);
            }
            VariantClear(&value);
            object->Release();
        }
        enumerator->Release();
        return result;
    }

private:
    void Initialize() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) com_initialized_ = true;
        else if (hr != RPC_E_CHANGED_MODE) return;

        hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE, nullptr);
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) return;

        IWbemLocator* locator = nullptr;
        hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, reinterpret_cast<void**>(&locator));
        if (FAILED(hr) || !locator) return;

        BSTR ns = SysAllocString(L"ROOT\\WMI");
        if (!ns) {
            locator->Release();
            return;
        }
        hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services_);
        SysFreeString(ns);
        locator->Release();
        if (FAILED(hr) || !services_) {
            services_ = nullptr;
            return;
        }

        hr = CoSetProxyBlanket(services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
            nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE);
        if (FAILED(hr)) {
            services_->Release();
            services_ = nullptr;
        }
    }

    bool com_initialized_ = false;
    IWbemServices* services_ = nullptr;
};

DisplayReading ReadDisplay(const BrightnessReader& brightness) {
    DisplayReading result;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        result.width = static_cast<int>(mode.dmPelsWidth);
        result.height = static_cast<int>(mode.dmPelsHeight);
        result.refresh_hz = static_cast<int>(mode.dmDisplayFrequency);
    }

    UINT32 path_count = 0, mode_count = 0;
    LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (status == ERROR_SUCCESS && path_count > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
            &mode_count, modes.data(), nullptr);
        if (status == ERROR_SUCCESS) {
            result.active_displays = static_cast<int>(path_count);
            for (UINT32 i = 0; i < path_count; ++i) {
                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
                info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
                info.header.size = sizeof(info);
                info.header.adapterId = paths[i].targetInfo.adapterId;
                info.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
                    result.hdr_supported = result.hdr_supported || info.advancedColorSupported;
                    result.hdr_enabled = result.hdr_enabled || info.advancedColorEnabled;
                }
            }
        }
    }
    result.brightness_percent = brightness.Read();
    return result;
}

std::wstring ReadPowerSchemeName() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return L"未知";
    DWORD bytes = 0;
    PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, nullptr, &bytes);
    std::wstring result = L"未知";
    if (bytes > sizeof(wchar_t)) {
        std::vector<UCHAR> buffer(bytes);
        if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr,
                buffer.data(), &bytes) == ERROR_SUCCESS) {
            result = reinterpret_cast<const wchar_t*>(buffer.data());
        }
    }
    LocalFree(scheme);
    return result;
}

struct StaticSystemReading {
    DisplayReading display;
    std::wstring power_scheme = L"正在读取";
};

class StaticSystemSampler {
public:
    StaticSystemSampler() : worker_([this](std::stop_token stop) { Run(stop); }) {}

    StaticSystemReading Get() const {
        std::scoped_lock lock(mutex_);
        return reading_;
    }

private:
    void Run(std::stop_token stop) {
        // WMI can occasionally block for hundreds of milliseconds. Keep these
        // slow-changing probes off the main refresh loop so the dashboard stays smooth.
        BrightnessReader brightness;
        while (!stop.stop_requested()) {
            StaticSystemReading next;
            next.display = ReadDisplay(brightness);
            next.power_scheme = ReadPowerSchemeName();
            {
                std::scoped_lock lock(mutex_);
                reading_ = std::move(next);
            }
            for (int i = 0; i < 30 && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    mutable std::mutex mutex_;
    StaticSystemReading reading_;
    std::jthread worker_;
};

// 仅声明本程序使用的 NVML ABI 子集，运行时从 NVIDIA 驱动自带的 nvml.dll 动态加载。
using nvmlReturn_t = int;
struct nvmlDevice_st;
using nvmlDevice_t = nvmlDevice_st*;
struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr unsigned int NVML_TEMPERATURE_GPU = 0;
constexpr unsigned int NVML_CLOCK_GRAPHICS = 0;
constexpr unsigned int NVML_CLOCK_MEM = 2;

struct GpuReading {
    bool library_available = false;
    bool device_available = false;
    bool data_available = false;
    std::wstring name;
    double power_w = std::numeric_limits<double>::quiet_NaN();
    int utilization = -1;
    int memory_utilization = -1;
    int temperature_c = -1;
    int graphics_mhz = -1;
    int memory_mhz = -1;
    int pstate = -1;
};

class NvmlSampler {
public:
    NvmlSampler() { Initialize(); }
    ~NvmlSampler() {
        if (shutdown_ && initialized_) shutdown_();
        if (module_) FreeLibrary(module_);
    }

    bool Available() const { return initialized_ && device_ != nullptr; }

    GpuReading Sample() const {
        GpuReading result;
        result.library_available = module_ != nullptr;
        result.device_available = Available();
        result.name = name_;
        if (!Available()) return result;

        bool any = false;
        unsigned int value = 0;
        if (get_power_usage_ && get_power_usage_(device_, &value) == NVML_SUCCESS) {
            result.power_w = static_cast<double>(value) / 1000.0;
            any = true;
        }
        nvmlUtilization_t utilization{};
        if (get_utilization_ && get_utilization_(device_, &utilization) == NVML_SUCCESS) {
            result.utilization = static_cast<int>(utilization.gpu);
            result.memory_utilization = static_cast<int>(utilization.memory);
            any = true;
        }
        if (get_temperature_ && get_temperature_(device_, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS) {
            result.temperature_c = static_cast<int>(value);
            any = true;
        }
        if (get_clock_info_ && get_clock_info_(device_, NVML_CLOCK_GRAPHICS, &value) == NVML_SUCCESS) {
            result.graphics_mhz = static_cast<int>(value);
            any = true;
        }
        if (get_clock_info_ && get_clock_info_(device_, NVML_CLOCK_MEM, &value) == NVML_SUCCESS) {
            result.memory_mhz = static_cast<int>(value);
            any = true;
        }
        if (get_pstate_ && get_pstate_(device_, &value) == NVML_SUCCESS) {
            result.pstate = static_cast<int>(value);
            any = true;
        }
        result.data_available = any;
        return result;
    }

private:
    template <typename T>
    T Load(const char* name) {
        return reinterpret_cast<T>(GetProcAddress(module_, name));
    }

    void Initialize() {
        module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) module_ = LoadLibraryW(L"nvml.dll");
        if (!module_) {
            module_ = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        }
        if (!module_) return;

        init_ = Load<InitFn>("nvmlInit_v2");
        shutdown_ = Load<ShutdownFn>("nvmlShutdown");
        get_count_ = Load<GetCountFn>("nvmlDeviceGetCount_v2");
        get_handle_ = Load<GetHandleFn>("nvmlDeviceGetHandleByIndex_v2");
        get_name_ = Load<GetNameFn>("nvmlDeviceGetName");
        get_power_usage_ = Load<GetUIntFn>("nvmlDeviceGetPowerUsage");
        get_utilization_ = Load<GetUtilizationFn>("nvmlDeviceGetUtilizationRates");
        get_temperature_ = Load<GetTemperatureFn>("nvmlDeviceGetTemperature");
        get_clock_info_ = Load<GetClockFn>("nvmlDeviceGetClockInfo");
        get_pstate_ = Load<GetUIntFn>("nvmlDeviceGetPerformanceState");
        if (!init_ || !shutdown_ || !get_count_ || !get_handle_) return;
        if (init_() != NVML_SUCCESS) return;
        initialized_ = true;

        unsigned int count = 0;
        if (get_count_(&count) != NVML_SUCCESS || count == 0) return;
        if (get_handle_(0, &device_) != NVML_SUCCESS) {
            device_ = nullptr;
            return;
        }
        if (get_name_) {
            char name[128]{};
            if (get_name_(device_, name, static_cast<unsigned int>(sizeof(name))) == NVML_SUCCESS) {
                const int required = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
                if (required > 1) {
                    std::wstring wide(static_cast<size_t>(required), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), required);
                    if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
                    name_ = std::move(wide);
                }
            }
        }
        if (name_.empty()) name_ = L"NVIDIA GPU";
    }

    using InitFn = nvmlReturn_t (*)();
    using ShutdownFn = nvmlReturn_t (*)();
    using GetCountFn = nvmlReturn_t (*)(unsigned int*);
    using GetHandleFn = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
    using GetNameFn = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
    using GetUIntFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
    using GetUtilizationFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
    using GetTemperatureFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);
    using GetClockFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);

    HMODULE module_ = nullptr;
    bool initialized_ = false;
    nvmlDevice_t device_ = nullptr;
    std::wstring name_;
    InitFn init_ = nullptr;
    ShutdownFn shutdown_ = nullptr;
    GetCountFn get_count_ = nullptr;
    GetHandleFn get_handle_ = nullptr;
    GetNameFn get_name_ = nullptr;
    GetUIntFn get_power_usage_ = nullptr;
    GetUtilizationFn get_utilization_ = nullptr;
    GetTemperatureFn get_temperature_ = nullptr;
    GetClockFn get_clock_info_ = nullptr;
    GetUIntFn get_pstate_ = nullptr;
};

class PowerHistory {
public:
    void Reset() { values_.clear(); }

    void Add(Clock::time_point now, double value) {
        if (!std::isfinite(value)) {
            Prune(now);
            return;
        }
        values_.emplace_back(now, value);
        Prune(now);
    }

    double Average(Clock::time_point now, std::chrono::seconds window) const {
        if (values_.empty()) return std::numeric_limits<double>::quiet_NaN();
        const auto threshold = now - window;
        double sum = 0.0;
        size_t count = 0;
        for (auto it = values_.rbegin(); it != values_.rend(); ++it) {
            if (it->first < threshold) break;
            sum += it->second;
            ++count;
        }
        return count == 0 ? std::numeric_limits<double>::quiet_NaN()
                          : sum / static_cast<double>(count);
    }

    int CoverageSeconds(Clock::time_point now) const {
        if (values_.empty()) return 0;
        return static_cast<int>(std::clamp(
            std::chrono::duration_cast<std::chrono::seconds>(now - values_.front().first).count(),
            static_cast<long long>(0), static_cast<long long>(60)));
    }

private:
    void Prune(Clock::time_point now) {
        const auto threshold = now - std::chrono::seconds(60);
        while (!values_.empty() && values_.front().first < threshold) values_.pop_front();
    }

    std::deque<std::pair<Clock::time_point, double>> values_;
};

std::vector<std::wstring> Diagnose(const BatteryReading& battery, const CpuReading& cpu,
    const GpuReading& gpu, const DisplayReading& display,
    const ProcessSampleResult& processes, const NetworkReading& network) {
    std::vector<std::wstring> messages;
    if (std::isfinite(battery.system_draw_w)) {
        if (battery.system_draw_w >= 40.0)
            messages.emplace_back(L"严重：整机放电功耗超过 40 W，99.9 Wh 电池续航通常不足约 2.5 小时。");
        else if (battery.system_draw_w >= 25.0)
            messages.emplace_back(L"偏高：整机放电功耗超过 25 W，轻办公不应长期维持在此水平。");
        else if (battery.system_draw_w >= 17.0)
            messages.emplace_back(L"注意：当前整机功耗偏高，请结合 30/60 秒均值判断是否持续。");
    }
    if (gpu.data_available && std::isfinite(gpu.power_w)) {
        if (gpu.power_w >= 5.0 && gpu.utilization >= 0 && gpu.utilization <= 5)
            messages.emplace_back(L"独显疑似空闲未休眠：利用率很低，但仍有明显功耗。");
        else if (gpu.power_w >= 10.0)
            messages.emplace_back(L"RTX 独显当前贡献了较明显的功耗。");
    }
    if (std::isfinite(cpu.utilization) && cpu.utilization >= 20.0)
        messages.emplace_back(L"CPU 总占用持续偏高；查看活跃进程排行定位后台任务。");
    if (display.brightness_percent >= 75)
        messages.emplace_back(L"内屏亮度较高，是整机未归因功耗的重要来源。");
    if (display.refresh_hz >= 120)
        messages.emplace_back(L"屏幕处于高刷新率；续航测试时建议切换到 60 Hz 对比。");
    if (display.hdr_enabled)
        messages.emplace_back(L"HDR/高级颜色已开启，可能提高显示链路与背光功耗。");
    if (processes.total_read_mib_s + processes.total_write_mib_s >= 10.0)
        messages.emplace_back(L"进程磁盘 I/O 较高，可能造成 CPU、SSD 和后台服务持续活跃。");
    if (network.receive_mib_s + network.send_mib_s >= 5.0)
        messages.emplace_back(L"网络吞吐量较高，可能存在下载、同步、更新或代理流量。");
    if (messages.empty()) messages.emplace_back(L"当前未命中明显异常阈值；优先观察 30 秒和 60 秒平均功耗。");
    return messages;
}

std::wstring OnOff(bool value) { return value ? L"开" : L"关"; }

std::wstring ValueOrNA(int value, const wchar_t* suffix = L"") {
    return value >= 0 ? std::to_wstring(value) + suffix : L"N/A";
}

std::vector<std::wstring> BuildDashboard(const Options& options, const std::wstring& scheme,
    const BatteryReading& battery, double avg5, double avg30, double avg60, int coverage_seconds,
    const CpuReading& cpu, const GpuReading& gpu, const MemoryReading& memory,
    const ProcessSampleResult& processes, const NetworkReading& network,
    const DisplayReading& display, const std::vector<std::wstring>& diagnosis) {
    std::vector<std::wstring> lines;
    lines.reserve(24);
    lines.emplace_back(L"PowerScope 0.3  |  " + NowLocalText() + L"  |  刷新 " +
        std::to_wstring(options.interval_ms) + L" ms  |  Ctrl+C 退出");
    lines.emplace_back(L"────────────────────────────────────────────────────────────────────────────────────────────");

    if (std::isfinite(battery.system_draw_w)) {
        lines.emplace_back(L"整机 [实测]  " + FormatMaybe(battery.system_draw_w) + L" W   |   5秒 " +
            FormatMaybe(avg5) + L" W   |   30秒 " + FormatMaybe(avg30) + L" W   |   60秒 " +
            FormatMaybe(avg60) + L" W");
    } else if (battery.ac_online) {
        std::wstring rate = std::isfinite(battery.signed_rate_w)
            ? L"；电池充放电率 " + FormatMaybe(battery.signed_rate_w) + L" W" : L"";
        lines.emplace_back(L"整机功耗 N/A：插电时公开 Windows API 无法测量适配器侧输入功率" + rate);
    } else {
        lines.emplace_back(L"整机功耗 N/A：电池驱动未提供有效放电率");
    }

    lines.emplace_back(L"电池  " + (battery.ac_online ? std::wstring(L"外接电源") : std::wstring(L"放电")) +
        L"  |  电量 " + (battery.percent >= 0 ? std::to_wstring(battery.percent) + L"%" : L"未知") +
        L"  |  预计 " + FormatDuration(battery.estimated_seconds) +
        L"  |  剩余 " + FormatMaybe(battery.remaining_wh) + L" Wh  |  节电 " + OnOff(battery.saver));

    lines.emplace_back(L"CPU   " + FormatMaybe(cpu.utilization) + L"%  |  频率 " +
        FormatMaybe(cpu.average_mhz / 1000.0, 2) + L" / " +
        FormatMaybe(cpu.average_limit_mhz / 1000.0, 2) + L" GHz  |  内存 " +
        FormatMaybe(memory.used_gib, 1) + L" / " + FormatMaybe(memory.total_gib, 1) +
        L" GiB (" + FormatMaybe(memory.used_percent, 0) + L"%)");

    if (!options.gpu_enabled) {
        lines.emplace_back(L"GPU   NVML 已禁用（--no-gpu），用于验证查询是否影响独显休眠");
    } else if (!gpu.library_available) {
        lines.emplace_back(L"GPU   未找到 NVIDIA NVML，请确认 NVIDIA 驱动已正确安装");
    } else if (!gpu.device_available) {
        lines.emplace_back(L"GPU   NVML 已加载，但未取得 NVIDIA GPU 句柄");
    } else if (!gpu.data_available) {
        lines.emplace_back(L"GPU   " + gpu.name + L"：无传感器数据，可能处于深度休眠");
    } else {
        lines.emplace_back(L"GPU   " + gpu.name + L"  |  " + FormatMaybe(gpu.power_w) + L" W  |  利用率 " +
            ValueOrNA(gpu.utilization, L"%") + L"  |  显存 " + ValueOrNA(gpu.memory_utilization, L"%") +
            L"  |  " + ValueOrNA(gpu.temperature_c, L"°C") + L"  |  " +
            ValueOrNA(gpu.graphics_mhz, L" MHz") + L" / " + ValueOrNA(gpu.memory_mhz, L" MHz") +
            L"  |  P" + (gpu.pstate >= 0 ? std::to_wstring(gpu.pstate) : L"?"));
    }

    lines.emplace_back(L"显示  " + std::to_wstring(display.width) + L"×" + std::to_wstring(display.height) +
        L" @ " + std::to_wstring(display.refresh_hz) + L" Hz  |  亮度 " +
        ValueOrNA(display.brightness_percent, L"%") + L"  |  HDR " + OnOff(display.hdr_enabled) +
        L"  |  活跃显示器 " + std::to_wstring(display.active_displays) + L"  |  方案 " + scheme);

    lines.emplace_back(L"活动  网络 ↓" + FormatMaybe(network.receive_mib_s, 2) + L" ↑" +
        FormatMaybe(network.send_mib_s, 2) + L" MiB/s  |  进程 I/O 读 " +
        FormatMaybe(processes.total_read_mib_s, 2) + L" 写 " +
        FormatMaybe(processes.total_write_mib_s, 2) + L" MiB/s");

    lines.emplace_back(L"──────────────────────────────── 活跃进程 ────────────────────────────────");
    lines.emplace_back(L"  " + PadRightCells(L"进程", 30) + PadLeftCells(L"PID", 8) +
        PadLeftCells(L"CPU%", 9) + PadLeftCells(L"读 MiB/s", 13) + PadLeftCells(L"写 MiB/s", 13));

    constexpr size_t kProcessRows = 6;
    for (size_t i = 0; i < kProcessRows; ++i) {
        if (i < processes.top.size()) {
            const auto& p = processes.top[i];
            lines.emplace_back(L"  " + PadRightCells(p.name, 30) +
                PadLeftCells(std::to_wstring(p.pid), 8) +
                PadLeftCells(FormatMaybe(p.cpu_percent, 1), 9) +
                PadLeftCells(FormatMaybe(p.read_mib_s, 2), 13) +
                PadLeftCells(FormatMaybe(p.write_mib_s, 2), 13));
        } else {
            lines.emplace_back(L"");
        }
    }

    lines.emplace_back(L"──────────────────────────────── 自动判断 ────────────────────────────────");
    constexpr size_t kDiagnosisRows = 4;
    for (size_t i = 0; i < kDiagnosisRows; ++i) {
        if (i < diagnosis.size()) {
            if (i == kDiagnosisRows - 1 && diagnosis.size() > kDiagnosisRows) {
                lines.emplace_back(L"  • " + diagnosis[i] + L"  （另有 " +
                    std::to_wstring(diagnosis.size() - kDiagnosisRows) + L" 条）");
            } else {
                lines.emplace_back(L"  • " + diagnosis[i]);
            }
        } else {
            lines.emplace_back(L"");
        }
    }

    std::wstring footer = L"[实测] 传感器直接报告  [指标] 活动程度  [状态] 配置；";
    if (coverage_seconds < 60 && std::isfinite(battery.system_draw_w)) {
        footer += L"60秒均值预热 " + std::to_wstring(coverage_seconds) + L"/60 秒；";
    }
    footer += options.gpu_enabled
        ? L"NVML 在部分混合显卡笔记本上可能唤醒独显，可用 --no-gpu 对照。"
        : L"当前未查询 NVML。";
    lines.emplace_back(std::move(footer));
    return lines;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    InitializeConsoleOutput();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    const auto parsed = ParseOptions(argc, argv);
    if (!parsed) return argc > 1 ? 0 : 1;
    const Options options = *parsed;

    std::wcout << L"PowerScope 正在初始化监测器，请稍候……" << std::flush;

    CpuSampler cpu_sampler;
    ProcessSampler process_sampler(cpu_sampler.LogicalProcessors());
    NetworkSampler network_sampler;
    StaticSystemSampler static_system_sampler;
    NvmlSampler nvml_sampler;
    PowerHistory power_history;

    StaticSystemReading static_system = static_system_sampler.Get();
    GpuReading gpu;
    auto last_gpu_sample = Clock::now() - std::chrono::milliseconds(options.gpu_interval_ms);

    // Establish process/network baselines before the dashboard starts.
    process_sampler.Sample(1.0);
    network_sampler.Sample(1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));

    {
        TerminalRenderer renderer;
        auto last_sample = Clock::now();
        auto next_tick = last_sample;
        bool was_on_ac = false;

        while (!g_stop.load()) {
            next_tick += std::chrono::milliseconds(options.interval_ms);
            const auto cycle_start = Clock::now();
            double elapsed = std::chrono::duration<double>(cycle_start - last_sample).count();
            if (elapsed <= 0.0) elapsed = options.interval_ms / 1000.0;
            last_sample = cycle_start;

            const BatteryReading battery = ReadBattery();
            const CpuReading cpu = cpu_sampler.Sample();
            const ProcessSampleResult processes = process_sampler.Sample(elapsed);
            const NetworkReading network = network_sampler.Sample(elapsed);
            const MemoryReading memory = ReadMemory();

            if (options.gpu_enabled &&
                cycle_start - last_gpu_sample >= std::chrono::milliseconds(options.gpu_interval_ms)) {
                gpu = nvml_sampler.Sample();
                last_gpu_sample = cycle_start;
            }
            static_system = static_system_sampler.Get();

            if (battery.ac_online && !was_on_ac) power_history.Reset();
            was_on_ac = battery.ac_online;
            if (!battery.ac_online) power_history.Add(cycle_start, battery.system_draw_w);

            const double avg5 = power_history.Average(cycle_start, std::chrono::seconds(5));
            const double avg30 = power_history.Average(cycle_start, std::chrono::seconds(30));
            const double avg60 = power_history.Average(cycle_start, std::chrono::seconds(60));
            const int coverage = power_history.CoverageSeconds(cycle_start);
            const auto diagnosis = Diagnose(battery, cpu, gpu, static_system.display, processes, network);
            renderer.Render(BuildDashboard(options, static_system.power_scheme, battery, avg5, avg30, avg60,
                coverage, cpu, gpu, memory, processes, network, static_system.display, diagnosis));

            const auto now = Clock::now();
            if (next_tick <= now) next_tick = now;
            while (!g_stop.load() && Clock::now() < next_tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
    }

    std::wcout << L"\n已停止监控。\n";
    return 0;
}
