#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace powerscope {

inline double MissingNumber() {
    return std::numeric_limits<double>::quiet_NaN();
}

enum class DataStatus {
    Available,
    Unmeasurable,
    TemporarilyUnavailable,
    Disabled,
    WarmingUp,
};

struct Options {
    std::uint32_t interval_ms = 1000;
    std::uint32_t gpu_interval_ms = 5000;
    bool gpu_enabled = true;
};

struct BatteryReading {
    bool available = false;
    bool ac_online = false;
    bool charging = false;
    bool discharging = false;
    bool saver = false;
    int percent = -1;
    double signed_rate_w = MissingNumber();
    double system_draw_w = MissingNumber();
    double remaining_wh = MissingNumber();
    double max_wh = MissingNumber();
    std::uint32_t estimated_seconds = static_cast<std::uint32_t>(-1);
    DataStatus estimate_status = DataStatus::TemporarilyUnavailable;
    DataStatus draw_status = DataStatus::TemporarilyUnavailable;
};

struct CpuReading {
    DataStatus status = DataStatus::TemporarilyUnavailable;
    DataStatus utilization_status = DataStatus::TemporarilyUnavailable;
    DataStatus frequency_status = DataStatus::TemporarilyUnavailable;
    double utilization = MissingNumber();
    double average_mhz = MissingNumber();
    double average_max_mhz = MissingNumber();
    double average_limit_mhz = MissingNumber();
};

struct ProcessReading {
    std::uint32_t pid = 0;
    std::wstring name;
    double cpu_percent = 0.0;
    double read_mib_s = 0.0;
    double write_mib_s = 0.0;
};

struct ProcessReadingSet {
    std::vector<ProcessReading> top;
    double total_read_mib_s = 0.0;
    double total_write_mib_s = 0.0;
};

struct NetworkReading {
    DataStatus status = DataStatus::TemporarilyUnavailable;
    double receive_mib_s = 0.0;
    double send_mib_s = 0.0;
};

struct MemoryReading {
    DataStatus status = DataStatus::TemporarilyUnavailable;
    double used_percent = MissingNumber();
    double used_gib = MissingNumber();
    double total_gib = MissingNumber();
};

struct DisplayReading {
    DataStatus status = DataStatus::TemporarilyUnavailable;
    int width = 0;
    int height = 0;
    int refresh_hz = 0;
    int active_displays = 0;
    bool hdr_supported = false;
    bool hdr_enabled = false;
    DataStatus hdr_status = DataStatus::TemporarilyUnavailable;
    int brightness_percent = -1;
    DataStatus brightness_status = DataStatus::TemporarilyUnavailable;
};

struct GpuReading {
    DataStatus status = DataStatus::TemporarilyUnavailable;
    std::wstring name = L"NVIDIA GPU";
    double power_w = MissingNumber();
    int utilization = -1;
    int memory_utilization = -1;
    int temperature_c = -1;
    int graphics_mhz = -1;
    int memory_mhz = -1;
    int pstate = -1;
};

struct PowerAverages {
    double seconds_5 = MissingNumber();
    double seconds_30 = MissingNumber();
    double seconds_60 = MissingNumber();
    int coverage_seconds = 0;
};

struct ObservationSnapshot {
    Options options;
    std::wstring timestamp;
    std::wstring power_scheme;
    BatteryReading battery;
    CpuReading cpu;
    GpuReading gpu;
    MemoryReading memory;
    ProcessReadingSet processes;
    NetworkReading network;
    DisplayReading display;
    PowerAverages power_averages;
};

} // namespace powerscope
