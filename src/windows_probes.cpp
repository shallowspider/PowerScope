#include "windows_probes.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <powrprof.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <oleauto.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace powerscope {
namespace {

constexpr double kBytesPerMiB = 1024.0 * 1024.0;

std::uint64_t FileTimeToU64(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

std::wstring NowLocalText() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, std::size(buffer), L"%04u-%02u-%02u %02u:%02u:%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

BatteryReading ReadBattery() {
    BatteryReading result;
    SYSTEM_POWER_STATUS basic{};
    if (GetSystemPowerStatus(&basic)) {
        result.ac_online = basic.ACLineStatus == 1;
        result.saver = basic.SystemStatusFlag != 0;
        if (basic.BatteryLifePercent != 255) result.percent = basic.BatteryLifePercent;
        result.estimated_seconds = basic.BatteryLifeTime;
        if (basic.BatteryLifeTime != static_cast<DWORD>(-1) && basic.BatteryLifeTime != 0)
            result.estimate_status = DataStatus::Available;
    }
    SYSTEM_BATTERY_STATE state{};
    if (CallNtPowerInformation(SystemBatteryState, nullptr, 0, &state, sizeof(state)) == 0 && state.BatteryPresent) {
        result.available = true;
        result.ac_online = state.AcOnLine != FALSE;
        result.charging = state.Charging != FALSE;
        result.discharging = state.Discharging != FALSE;
        const auto signed_rate_mw = static_cast<LONG>(state.Rate);
        result.signed_rate_w = static_cast<double>(signed_rate_mw) / 1000.0;
        if ((result.discharging || !result.ac_online) && signed_rate_mw < 0) {
            result.system_draw_w = -result.signed_rate_w;
            result.draw_status = DataStatus::Available;
        } else {
            result.draw_status = result.ac_online ? DataStatus::Unmeasurable : DataStatus::TemporarilyUnavailable;
        }
        if (state.RemainingCapacity != 0) result.remaining_wh = state.RemainingCapacity / 1000.0;
        if (state.MaxCapacity != 0) result.max_wh = state.MaxCapacity / 1000.0;
        if (state.EstimatedTime != static_cast<ULONG>(-1) && state.EstimatedTime != 0)
            { result.estimated_seconds = state.EstimatedTime; result.estimate_status = DataStatus::Available; }
    } else {
        result.draw_status = result.ac_online ? DataStatus::Unmeasurable : DataStatus::TemporarilyUnavailable;
    }
    if (result.estimate_status != DataStatus::Available)
        result.estimate_status = result.ac_online ? DataStatus::Unmeasurable : DataStatus::TemporarilyUnavailable;
    return result;
}

struct ProcessorPowerInformationLocal {
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};

class CpuSampler {
public:
    CpuSampler() : logical_processors_(std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))) {
        ReadTimes();
    }

    CpuReading Sample() {
        CpuReading result;
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const auto idle_now = FileTimeToU64(idle);
            const auto kernel_now = FileTimeToU64(kernel);
            const auto user_now = FileTimeToU64(user);
            if (has_times_) {
                const auto idle_delta = idle_now - idle_;
                const auto kernel_delta = kernel_now - kernel_;
                const auto user_delta = user_now - user_;
                const auto total = kernel_delta + user_delta;
                if (total > 0 && total >= idle_delta)
                    { result.utilization = 100.0 * static_cast<double>(total - idle_delta) / total;
                      result.utilization_status = DataStatus::Available; }
            }
            idle_ = idle_now; kernel_ = kernel_now; user_ = user_now; has_times_ = true;
        }
        std::vector<ProcessorPowerInformationLocal> info(logical_processors_);
        if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, info.data(),
                static_cast<ULONG>(info.size() * sizeof(info[0]))) == 0 && !info.empty()) {
            double current = 0, maximum = 0, limit = 0;
            for (const auto& cpu : info) { current += cpu.CurrentMhz; maximum += cpu.MaxMhz; limit += cpu.MhzLimit; }
            result.average_mhz = current / info.size();
            result.average_max_mhz = maximum / info.size();
            result.average_limit_mhz = limit / info.size();
            result.frequency_status = DataStatus::Available;
        }
        if (std::isfinite(result.utilization) || std::isfinite(result.average_mhz))
            result.status = DataStatus::Available;
        return result;
    }

    DWORD LogicalProcessors() const { return logical_processors_; }

private:
    void ReadTimes() {
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            idle_ = FileTimeToU64(idle); kernel_ = FileTimeToU64(kernel); user_ = FileTimeToU64(user); has_times_ = true;
        }
    }
    DWORD logical_processors_ = 1;
    bool has_times_ = false;
    std::uint64_t idle_ = 0, kernel_ = 0, user_ = 0;
};

class ProcessSampler {
public:
    explicit ProcessSampler(DWORD processors) : processors_(std::max<DWORD>(1, processors)) {}

    ProcessReadingSet Sample(double elapsed) {
        ProcessReadingSet output;
        if (elapsed <= 0) return output;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return output;
        std::unordered_map<DWORD, Previous> next;
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                const DWORD pid = entry.th32ProcessID;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!process) continue;
                FILETIME creation{}, exit{}, kernel{}, user{}; IO_COUNTERS io{};
                const bool time_ok = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
                const bool io_ok = GetProcessIoCounters(process, &io) != FALSE;
                CloseHandle(process);
                if (!time_ok) continue;
                Previous current{FileTimeToU64(creation), FileTimeToU64(kernel) + FileTimeToU64(user),
                    io_ok ? io.ReadTransferCount : 0, io_ok ? io.WriteTransferCount : 0, entry.szExeFile};
                next.emplace(pid, current);
                const auto old = previous_.find(pid);
                if (old == previous_.end() || old->second.creation != current.creation) continue;
                ProcessReading reading{pid, current.name};
                const auto cpu_delta = current.cpu >= old->second.cpu ? current.cpu - old->second.cpu : 0;
                reading.cpu_percent = (static_cast<double>(cpu_delta) / 10'000'000.0) / elapsed / processors_ * 100.0;
                if (io_ok) {
                    reading.read_mib_s = static_cast<double>(current.read >= old->second.read ? current.read - old->second.read : 0) / kBytesPerMiB / elapsed;
                    reading.write_mib_s = static_cast<double>(current.write >= old->second.write ? current.write - old->second.write : 0) / kBytesPerMiB / elapsed;
                    output.total_read_mib_s += reading.read_mib_s;
                    output.total_write_mib_s += reading.write_mib_s;
                }
                if (reading.cpu_percent >= 0.05 || reading.read_mib_s + reading.write_mib_s >= 0.05)
                    output.top.push_back(std::move(reading));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        previous_ = std::move(next);
        std::ranges::sort(output.top, [](const auto& left, const auto& right) {
            const auto score = [](const auto& value) { return value.cpu_percent + std::min(20.0, value.read_mib_s + value.write_mib_s); };
            return score(left) > score(right);
        });
        if (output.top.size() > 6) output.top.resize(6);
        return output;
    }

private:
    struct Previous { std::uint64_t creation, cpu, read, write; std::wstring name; };
    DWORD processors_;
    std::unordered_map<DWORD, Previous> previous_;
};

class NetworkSampler {
public:
    NetworkReading Sample(double elapsed) {
        NetworkReading result;
        const auto totals = ReadTotals();
        if (!totals.available) return result;
        result.status = DataStatus::Available;
        if (elapsed > 0 && has_previous_) {
            result.receive_mib_s = static_cast<double>(totals.in >= previous_in_ ? totals.in - previous_in_ : 0) / kBytesPerMiB / elapsed;
            result.send_mib_s = static_cast<double>(totals.out >= previous_out_ ? totals.out - previous_out_ : 0) / kBytesPerMiB / elapsed;
        }
        previous_in_ = totals.in; previous_out_ = totals.out; has_previous_ = true;
        return result;
    }
private:
    struct Totals { bool available = false; std::uint64_t in = 0, out = 0; };
    static Totals ReadTotals() {
        PMIB_IF_TABLE2 table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR || !table) return {};
        std::uint64_t in_hardware = 0, out_hardware = 0, in_fallback = 0, out_fallback = 0;
        bool found_hardware = false;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const auto& row = table->Table[i];
            if (row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            in_fallback += row.InOctets; out_fallback += row.OutOctets;
            if (row.InterfaceAndOperStatusFlags.HardwareInterface) {
                found_hardware = true; in_hardware += row.InOctets; out_hardware += row.OutOctets;
            }
        }
        FreeMibTable(table);
        return found_hardware ? Totals{true, in_hardware, out_hardware}
                              : Totals{true, in_fallback, out_fallback};
    }
    bool has_previous_ = false;
    std::uint64_t previous_in_ = 0, previous_out_ = 0;
};

MemoryReading ReadMemory() {
    MemoryReading result;
    MEMORYSTATUSEX state{}; state.dwLength = sizeof(state);
    if (GlobalMemoryStatusEx(&state)) {
        result.used_percent = state.dwMemoryLoad;
        result.total_gib = state.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
        result.used_gib = (state.ullTotalPhys - state.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
        result.status = DataStatus::Available;
    }
    return result;
}

class BrightnessReader {
public:
    BrightnessReader() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) com_initialized_ = true; else if (hr != RPC_E_CHANGED_MODE) return;
        hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) return;
        IWbemLocator* locator = nullptr;
        hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
            reinterpret_cast<void**>(&locator));
        if (FAILED(hr) || !locator) return;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        if (ns) { hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services_); SysFreeString(ns); }
        locator->Release();
        if (FAILED(hr) || !services_) { services_ = nullptr; return; }
        hr = CoSetProxyBlanket(services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        if (FAILED(hr)) { services_->Release(); services_ = nullptr; }
    }
    ~BrightnessReader() { if (services_) services_->Release(); if (com_initialized_) CoUninitialize(); }

    int Read() const {
        if (!services_) return -1;
        BSTR language = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
        if (!language || !query) { if (language) SysFreeString(language); if (query) SysFreeString(query); return -1; }
        IEnumWbemClassObject* enumerator = nullptr;
        const HRESULT hr = services_->ExecQuery(language, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
        SysFreeString(language); SysFreeString(query);
        if (FAILED(hr) || !enumerator) return -1;
        IWbemClassObject* object = nullptr; ULONG returned = 0; int result = -1;
        if (enumerator->Next(1000, 1, &object, &returned) == WBEM_S_NO_ERROR && returned == 1) {
            VARIANT value{}; VariantInit(&value);
            if (SUCCEEDED(object->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr))) {
                if (value.vt == VT_UI1) result = value.bVal;
                else if (value.vt == VT_I4) result = value.lVal;
                else if (value.vt == VT_UI4) result = static_cast<int>(value.ulVal);
            }
            VariantClear(&value); object->Release();
        }
        enumerator->Release();
        return result;
    }
private:
    bool com_initialized_ = false;
    IWbemServices* services_ = nullptr;
};

DisplayReading ReadDisplay(const BrightnessReader& brightness) {
    DisplayReading result;
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        result.width = static_cast<int>(mode.dmPelsWidth); result.height = static_cast<int>(mode.dmPelsHeight);
        result.refresh_hz = static_cast<int>(mode.dmDisplayFrequency);
        result.status = DataStatus::Available;
    }
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) == ERROR_SUCCESS && path_count > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count); std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) == ERROR_SUCCESS) {
            result.active_displays = static_cast<int>(path_count);
            bool advanced_color_read = false;
            for (UINT32 i = 0; i < path_count; ++i) {
                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
                info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO; info.header.size = sizeof(info);
                info.header.adapterId = paths[i].targetInfo.adapterId; info.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
                    advanced_color_read = true;
                    result.hdr_supported = result.hdr_supported || info.advancedColorSupported;
                    result.hdr_enabled = result.hdr_enabled || info.advancedColorEnabled;
                }
            }
            if (advanced_color_read) result.hdr_status = DataStatus::Available;
        }
    }
    result.brightness_percent = brightness.Read();
    result.brightness_status = result.brightness_percent >= 0
        ? DataStatus::Available : DataStatus::Unmeasurable;
    return result;
}

std::wstring ReadPowerSchemeName() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return L"未知";
    DWORD bytes = 0; PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, nullptr, &bytes);
    std::wstring result = L"未知";
    if (bytes > sizeof(wchar_t)) {
        std::vector<UCHAR> buffer(bytes);
        if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, buffer.data(), &bytes) == ERROR_SUCCESS)
            result = reinterpret_cast<const wchar_t*>(buffer.data());
    }
    LocalFree(scheme); return result;
}

struct SlowSystemReading { DisplayReading display; std::wstring scheme = L"正在读取"; };

class SlowSystemSampler {
public:
    SlowSystemSampler() : worker_([this](std::stop_token stop) {
        BrightnessReader brightness;
        while (!stop.stop_requested()) {
            SlowSystemReading next{ReadDisplay(brightness), ReadPowerSchemeName()};
            { std::scoped_lock lock(mutex_); reading_ = std::move(next); }
            for (int i = 0; i < 30 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }) {}
    SlowSystemReading Get() const { std::scoped_lock lock(mutex_); return reading_; }
private:
    mutable std::mutex mutex_;
    SlowSystemReading reading_;
    std::jthread worker_;
};

using nvmlReturn_t = int;
struct nvmlDevice_st; using nvmlDevice_t = nvmlDevice_st*;
struct nvmlUtilization_t { unsigned int gpu, memory; };
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr unsigned int NVML_TEMPERATURE_GPU = 0, NVML_CLOCK_GRAPHICS = 0, NVML_CLOCK_MEM = 2;

class NvmlSampler {
public:
    NvmlSampler() { Initialize(); }
    ~NvmlSampler() { if (shutdown_ && initialized_) shutdown_(); if (module_) FreeLibrary(module_); }

    GpuReading Sample() const {
        GpuReading result; result.name = name_;
        if (!module_) { result.status = DataStatus::Unmeasurable; return result; }
        if (!initialized_ || !device_) { result.status = DataStatus::TemporarilyUnavailable; return result; }
        bool any = false; unsigned int value = 0;
        if (power_ && power_(device_, &value) == NVML_SUCCESS) { result.power_w = value / 1000.0; any = true; }
        nvmlUtilization_t utilization{};
        if (utilization_ && utilization_(device_, &utilization) == NVML_SUCCESS) {
            result.utilization = static_cast<int>(utilization.gpu); result.memory_utilization = static_cast<int>(utilization.memory); any = true;
        }
        if (temperature_ && temperature_(device_, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS) { result.temperature_c = static_cast<int>(value); any = true; }
        if (clock_ && clock_(device_, NVML_CLOCK_GRAPHICS, &value) == NVML_SUCCESS) { result.graphics_mhz = static_cast<int>(value); any = true; }
        if (clock_ && clock_(device_, NVML_CLOCK_MEM, &value) == NVML_SUCCESS) { result.memory_mhz = static_cast<int>(value); any = true; }
        if (pstate_ && pstate_(device_, &value) == NVML_SUCCESS) { result.pstate = static_cast<int>(value); any = true; }
        result.status = any ? DataStatus::Available : DataStatus::TemporarilyUnavailable;
        return result;
    }
private:
    template <typename T> T Load(const char* name) { return reinterpret_cast<T>(GetProcAddress(module_, name)); }
    void Initialize() {
        module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) module_ = LoadLibraryW(L"nvml.dll");
        if (!module_) module_ = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        if (!module_) return;
        init_ = Load<InitFn>("nvmlInit_v2"); shutdown_ = Load<ShutdownFn>("nvmlShutdown");
        count_ = Load<CountFn>("nvmlDeviceGetCount_v2"); handle_ = Load<HandleFn>("nvmlDeviceGetHandleByIndex_v2");
        name_fn_ = Load<NameFn>("nvmlDeviceGetName"); power_ = Load<UIntFn>("nvmlDeviceGetPowerUsage");
        utilization_ = Load<UtilizationFn>("nvmlDeviceGetUtilizationRates"); temperature_ = Load<TemperatureFn>("nvmlDeviceGetTemperature");
        clock_ = Load<ClockFn>("nvmlDeviceGetClockInfo"); pstate_ = Load<UIntFn>("nvmlDeviceGetPerformanceState");
        if (!init_ || !shutdown_ || !count_ || !handle_ || init_() != NVML_SUCCESS) return;
        initialized_ = true; unsigned int count = 0;
        if (count_(&count) != NVML_SUCCESS || count == 0 || handle_(0, &device_) != NVML_SUCCESS) { device_ = nullptr; return; }
        if (name_fn_) {
            char name[128]{};
            if (name_fn_(device_, name, sizeof(name)) == NVML_SUCCESS) {
                const int required = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
                if (required > 1) { std::wstring wide(required, L'\0'); MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), required); wide.pop_back(); name_ = std::move(wide); }
            }
        }
        if (name_.empty()) name_ = L"NVIDIA GPU";
    }
    using InitFn = nvmlReturn_t (*)(); using ShutdownFn = nvmlReturn_t (*)();
    using CountFn = nvmlReturn_t (*)(unsigned int*); using HandleFn = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
    using NameFn = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int); using UIntFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
    using UtilizationFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
    using TemperatureFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);
    using ClockFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);
    HMODULE module_ = nullptr; bool initialized_ = false; nvmlDevice_t device_ = nullptr; std::wstring name_;
    InitFn init_ = nullptr; ShutdownFn shutdown_ = nullptr; CountFn count_ = nullptr; HandleFn handle_ = nullptr;
    NameFn name_fn_ = nullptr; UIntFn power_ = nullptr; UtilizationFn utilization_ = nullptr;
    TemperatureFn temperature_ = nullptr; ClockFn clock_ = nullptr; UIntFn pstate_ = nullptr;
};

} // namespace

struct WindowsObservationSource::Impl {
    Impl() : processes(cpu.LogicalProcessors()) {}
    CpuSampler cpu;
    ProcessSampler processes;
    NetworkSampler network;
    SlowSystemSampler slow_system;
    NvmlSampler nvml;
};

WindowsObservationSource::WindowsObservationSource() : impl_(std::make_unique<Impl>()) {}
WindowsObservationSource::~WindowsObservationSource() = default;

void WindowsObservationSource::Prime() {
    impl_->processes.Sample(1.0);
    impl_->network.Sample(1.0);
}

ObservationSnapshot WindowsObservationSource::SampleFast(double elapsed_seconds) {
    ObservationSnapshot result;
    result.timestamp = NowLocalText();
    result.battery = ReadBattery();
    result.cpu = impl_->cpu.Sample();
    result.processes = impl_->processes.Sample(elapsed_seconds);
    result.network = impl_->network.Sample(elapsed_seconds);
    result.memory = ReadMemory();
    const auto slow_reading = impl_->slow_system.Get();
    result.display = slow_reading.display;
    result.power_scheme = slow_reading.scheme;
    return result;
}

GpuReading WindowsObservationSource::SampleGpu() { return impl_->nvml.Sample(); }

WindowsSessionRuntime::WindowsSessionRuntime(const std::atomic_bool& stop) : stop_(stop) {}
SessionClock::time_point WindowsSessionRuntime::Now() { return SessionClock::now(); }
bool WindowsSessionRuntime::StopRequested() const { return stop_.load(); }
void WindowsSessionRuntime::WaitUntil(SessionClock::time_point deadline) {
    while (!stop_.load() && SessionClock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(25));
}

} // namespace powerscope
