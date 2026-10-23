// SPDX-License-Identifier: GPL-2.0-or-later
// PortGaGotoku: performance telemetry (bbport_perf.h). The system and GPU utilisation queries
// (NVML, Windows PDH, GetSystemTimes, GlobalMemoryStatusEx, VMA budgets) follow Mrsuss60's
// telemetry HUD (github.com/Mrsuss60/bloodborne_pc_windows_port, cdbf89b and 66a47ca,
// GPL-2.0-or-later).
#include "bbport_perf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <vk_mem_alloc.h>
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace BbPerf {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> hud_visible{false};
std::atomic<bool> recording{false};

// ---- GPU busy time: timestamps around every submission ------------------------------------------
constexpr std::uint32_t Slots = 512;
struct Slot {
    vk::CommandBuffer begin, end;
    bool pending = false;
};
std::mutex gpu_mutex;
vk::Device device;
vk::QueryPool query_pool;
vk::CommandPool command_pool;
double period_ns = 1.0;
Slot slots[Slots];
std::uint32_t next_slot = 0;
std::deque<std::uint32_t> pending_slots; // submission order
std::uint64_t gpu_busy_ns = 0;           // completed submissions, since start
VmaAllocator allocator = nullptr;
std::string gpu_name;

// Reads the results of finished submissions (oldest first). Called with gpu_mutex held.
void CollectGpu() {
    while (!pending_slots.empty()) {
        const std::uint32_t i = pending_slots.front();
        std::uint64_t data[4] = {};
        const vk::Result result = device.getQueryPoolResults(
            query_pool, 2 * i, 2, sizeof(data), data, 2 * sizeof(std::uint64_t),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (result != vk::Result::eSuccess || data[1] == 0 || data[3] == 0) {
            break;
        }
        if (data[2] > data[0]) {
            gpu_busy_ns += std::uint64_t(double(data[2] - data[0]) * period_ns);
        }
        slots[i].pending = false;
        pending_slots.pop_front();
    }
}

// ---- system telemetry (a sampler thread, once a second) --------------------------------------------
struct System {
    float cpu_util = 0.0f, gpu_util = -1.0f;
    float ram_used_gb = 0.0f, ram_total_gb = 0.0f;
    float vram_used_mb = 0.0f, vram_budget_mb = 0.0f;
    std::vector<ThreadLoad> threads; // all, busiest first
};
std::mutex system_mutex;
System system_now;

#ifdef _WIN32
// NVML (NVIDIA), loaded at run time.
struct NvmlUtilization {
    unsigned int gpu, memory;
};
using NvmlInit = int (*)();
using NvmlHandleByIndex = int (*)(unsigned int, void**);
using NvmlGetUtilization = int (*)(void*, NvmlUtilization*);
NvmlGetUtilization nvml_utilization = nullptr;
void* nvml_device = nullptr;

// PDH, for the other vendors: \GPU Engine(*engtype_3D)\Utilization Percentage, summed.
struct PdhItem {
    const wchar_t* name;
    std::uint32_t status;
    std::uint32_t pad;
    double value;
};
using PdhOpenQuery = long (*)(const wchar_t*, std::uintptr_t, void**);
using PdhAddEnglishCounter = long (*)(void*, const wchar_t*, std::uintptr_t, void**);
using PdhCollect = long (*)(void*);
using PdhGetArray = long (*)(void*, std::uint32_t, std::uint32_t*, std::uint32_t*, void*);
PdhCollect pdh_collect = nullptr;
PdhGetArray pdh_get_array = nullptr;
void* pdh_query = nullptr;
void* pdh_counter = nullptr;
std::vector<char> pdh_buffer;

void InitGpuUtilization() {
    if (HMODULE nvml = LoadLibraryA("nvml.dll")) {
        auto init = reinterpret_cast<NvmlInit>(GetProcAddress(nvml, "nvmlInit_v2"));
        auto handle = reinterpret_cast<NvmlHandleByIndex>(GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2"));
        nvml_utilization = reinterpret_cast<NvmlGetUtilization>(GetProcAddress(nvml, "nvmlDeviceGetUtilizationRates"));
        if (init && handle && nvml_utilization && init() == 0 && handle(0, &nvml_device) == 0 && nvml_device) {
            std::printf("Perf: GPU utilisation from NVML\n");
            return;
        }
        nvml_device = nullptr;
    }
    if (HMODULE pdh = LoadLibraryA("pdh.dll")) {
        auto open = reinterpret_cast<PdhOpenQuery>(GetProcAddress(pdh, "PdhOpenQueryW"));
        auto add = reinterpret_cast<PdhAddEnglishCounter>(GetProcAddress(pdh, "PdhAddEnglishCounterW"));
        pdh_collect = reinterpret_cast<PdhCollect>(GetProcAddress(pdh, "PdhCollectQueryData"));
        pdh_get_array = reinterpret_cast<PdhGetArray>(GetProcAddress(pdh, "PdhGetFormattedCounterArrayW"));
        if (open && add && pdh_collect && pdh_get_array && open(nullptr, 0, &pdh_query) == 0 &&
            add(pdh_query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &pdh_counter) == 0) {
            pdh_collect(pdh_query);
            std::printf("Perf: GPU utilisation from PDH\n");
        } else {
            pdh_counter = nullptr;
        }
    }
}

float GpuUtilization() {
    if (nvml_device) {
        NvmlUtilization rates{};
        return nvml_utilization(nvml_device, &rates) == 0 ? float(rates.gpu) : -1.0f;
    }
    if (pdh_counter && pdh_collect(pdh_query) == 0) {
        std::uint32_t size = 0, count = 0;
        pdh_get_array(pdh_counter, 0x200 /* PDH_FMT_DOUBLE */, &size, &count, nullptr);
        if (size) {
            pdh_buffer.resize(std::max<std::size_t>(pdh_buffer.size(), size));
            if (pdh_get_array(pdh_counter, 0x200, &size, &count, pdh_buffer.data()) == 0) {
                double total = 0.0;
                const auto* items = reinterpret_cast<const PdhItem*>(pdh_buffer.data());
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (items[i].status == 0 && items[i].value > 0.0) {
                        total += items[i].value;
                    }
                }
                return float(std::clamp(total, 0.0, 100.0));
            }
        }
    }
    return -1.0f;
}

std::uint64_t FileTime(const FILETIME& t) {
    return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

void SystemCpuAndRam(System& out) {
    static std::uint64_t last_idle = 0, last_total = 0;
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const std::uint64_t i = FileTime(idle), total = FileTime(kernel) + FileTime(user); // kernel includes idle
        if (last_total && total > last_total) {
            out.cpu_util = 100.0f * float(1.0 - double(i - last_idle) / double(total - last_total));
        }
        last_idle = i;
        last_total = total;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        out.ram_total_gb = float(memory.ullTotalPhys / 1073741824.0);
        out.ram_used_gb = float((memory.ullTotalPhys - memory.ullAvailPhys) / 1073741824.0);
    }
}

// CPU time (100 ns units) and name of every thread of the process.
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
void ThreadTimes(std::map<std::uint32_t, std::pair<std::uint64_t, std::string>>& out) {
    static const auto describe = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription"));
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self) {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            std::string name;
            PWSTR wide = nullptr;
            if (describe && SUCCEEDED(describe(thread, &wide)) && wide) {
                for (const wchar_t* c = wide; *c && name.size() < 48; ++c) {
                    name.push_back(*c < 128 ? char(*c) : '?');
                }
                LocalFree(wide);
            }
            out[entry.th32ThreadID] = {FileTime(kernel) + FileTime(user), name};
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}
#else
using NvmlInit = int (*)();
using NvmlHandleByIndex = int (*)(unsigned int, void**);
struct NvmlUtilization {
    unsigned int gpu, memory;
};
using NvmlGetUtilization = int (*)(void*, NvmlUtilization*);
NvmlGetUtilization nvml_utilization = nullptr;
void* nvml_device = nullptr;

void InitGpuUtilization() {
    if (void* nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW)) {
        auto init = reinterpret_cast<NvmlInit>(dlsym(nvml, "nvmlInit_v2"));
        auto handle = reinterpret_cast<NvmlHandleByIndex>(dlsym(nvml, "nvmlDeviceGetHandleByIndex_v2"));
        nvml_utilization = reinterpret_cast<NvmlGetUtilization>(dlsym(nvml, "nvmlDeviceGetUtilizationRates"));
        if (!(init && handle && nvml_utilization && init() == 0 && handle(0, &nvml_device) == 0)) {
            nvml_device = nullptr;
        }
    }
}

float GpuUtilization() {
    NvmlUtilization rates{};
    return nvml_device && nvml_utilization(nvml_device, &rates) == 0 ? float(rates.gpu) : -1.0f;
}

void SystemCpuAndRam(System& out) {
    static std::uint64_t last_idle = 0, last_total = 0;
    if (FILE* f = std::fopen("/proc/stat", "r")) {
        unsigned long long v[8] = {};
        if (std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
                        &v[5], &v[6], &v[7]) == 8) {
            const std::uint64_t idle = v[3] + v[4];
            std::uint64_t total = 0;
            for (unsigned long long x : v) {
                total += x;
            }
            if (last_total && total > last_total) {
                out.cpu_util = 100.0f * float(1.0 - double(idle - last_idle) / double(total - last_total));
            }
            last_idle = idle;
            last_total = total;
        }
        std::fclose(f);
    }
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        char key[64];
        unsigned long long kb = 0, total = 0, available = 0;
        while (std::fscanf(f, "%63s %llu kB", key, &kb) == 2) {
            if (!std::strcmp(key, "MemTotal:")) total = kb;
            if (!std::strcmp(key, "MemAvailable:")) available = kb;
        }
        std::fclose(f);
        out.ram_total_gb = float(total / 1048576.0);
        out.ram_used_gb = float((total - available) / 1048576.0);
    }
}

// CPU time (in 100 ns units, as on Windows) and name of every thread of the process.
void ThreadTimes(std::map<std::uint32_t, std::pair<std::uint64_t, std::string>>& out) {
    DIR* dir = opendir("/proc/self/task");
    if (!dir) {
        return;
    }
    const double ticks = double(sysconf(_SC_CLK_TCK));
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        const std::string base = std::string("/proc/self/task/") + e->d_name;
        std::string name;
        if (FILE* f = std::fopen((base + "/comm").c_str(), "r")) {
            char buffer[64] = "";
            if (std::fgets(buffer, sizeof(buffer), f)) {
                name = buffer;
                name.erase(name.find_last_not_of("\n") + 1);
            }
            std::fclose(f);
        }
        if (FILE* f = std::fopen((base + "/stat").c_str(), "r")) {
            char buffer[1024] = "";
            if (std::fgets(buffer, sizeof(buffer), f)) {
                // Fields after the ")" of the name: state is 3, utime 14, stime 15.
                const char* p = std::strrchr(buffer, ')');
                unsigned long long utime = 0, stime = 0;
                if (p && std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime,
                                     &stime) == 2) {
                    out[std::uint32_t(std::atoi(e->d_name))] = {
                        std::uint64_t(double(utime + stime) / ticks * 1e7), name};
                }
            }
            std::fclose(f);
        }
    }
    closedir(dir);
}
#endif

// ---- frames and seconds ----------------------------------------------------------------------------
std::mutex frame_mutex;
std::deque<std::pair<Clock::time_point, float>> recent_frames; // last 10 s
std::vector<float> history;                                    // the graph, oldest first
constexpr std::size_t HistorySize = 240;
Snapshot snapshot;

struct Window {
    Clock::time_point start{};
    std::uint32_t frames = 0;
    std::uint64_t cpu_us = 0, gpu_ns = 0, compiles = 0;
} window;
std::deque<std::pair<Clock::time_point, std::uint64_t>> compile_marks; // for "last 5 s"

// ---- the measurement ----------------------------------------------------------------------------------
std::mutex record_mutex;
FILE* frames_csv = nullptr;
FILE* seconds_csv = nullptr;
FILE* threads_csv = nullptr;
std::string record_base;
Clock::time_point record_start{};
int record_seconds = 0;
std::vector<float> record_frames;
std::uint64_t record_cpu_us = 0, record_gpu_ns = 0, record_compiles = 0;
std::uint32_t record_window_frames = 0; // frames of the completed seconds (the averages' denominator)
std::atomic<bool> restart_window{false};
double record_gpu_util_sum = 0.0, record_cpu_util_sum = 0.0;
std::uint32_t record_util_samples = 0, record_gpu_util_samples = 0;
std::map<std::uint32_t, std::pair<double, std::uint32_t>> record_thread_sum; // tid -> (percent sum, samples)
std::map<std::uint32_t, std::string> record_thread_names;

std::uint64_t CpuTranslateUs() {
    return BbStats::gpu_user_us.load(std::memory_order_relaxed) + BbStats::gpu_sys_us.load(std::memory_order_relaxed);
}

float Percentile(std::vector<float> values, double p) {
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = std::min(values.size() - 1, std::size_t(p / 100.0 * double(values.size())));
    return values[index];
}

// Average of the slowest `fraction` of frames, as FPS.
float LowFps(std::vector<float> values, double fraction) {
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end(), std::greater<>());
    const std::size_t n = std::max<std::size_t>(1, std::size_t(double(values.size()) * fraction));
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += values[i];
    }
    return sum > 0.0 ? float(1000.0 * double(n) / sum) : 0.0f;
}

std::string LogsDir() {
    const char* data = std::getenv("BB_DATA_DIR");
    return data && *data ? std::string(data) + "/logs" : std::string(".");
}

void FinishMeasurement() {
    std::scoped_lock lock{record_mutex};
    if (!frames_csv) {
        return;
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - record_start).count();
    const std::size_t frames = record_frames.size();
    double sum_ms = 0.0;
    for (float ms : record_frames) {
        sum_ms += ms;
    }
    std::vector<std::pair<double, std::uint32_t>> threads; // (average percent, tid)
    for (const auto& [tid, value] : record_thread_sum) {
        threads.emplace_back(value.second ? value.first / value.second : 0.0, tid);
    }
    std::sort(threads.begin(), threads.end(), std::greater<>());

    FILE* summary = std::fopen((record_base + "-summary.txt").c_str(), "w");
    const auto both = [summary](const char* format, auto... args) {
        std::printf(format, args...);
        if (summary) {
            std::fprintf(summary, format, args...);
        }
    };
    both("Perf summary: %.1f s, %zu frames\n", seconds, frames);
    if (sum_ms < 900.0 * seconds) {
        std::printf("Perf: frames presented for only %.1f of %.1f s (window minimised?): not a comparable "
                    "measurement\n",
                    sum_ms / 1000.0, seconds);
    }
    both("presented_s=%.1f\n", sum_ms / 1000.0);
    both("fps_avg=%.2f\n", frames && sum_ms > 0 ? 1000.0 * double(frames) / sum_ms : 0.0);
    both("fps_low1=%.2f\nfps_low01=%.2f\n", LowFps(record_frames, 0.01), LowFps(record_frames, 0.001));
    both("frame_ms_p50=%.2f\nframe_ms_p90=%.2f\nframe_ms_p95=%.2f\nframe_ms_p99=%.2f\nframe_ms_p999=%.2f\nframe_ms_max=%.2f\n",
         Percentile(record_frames, 50), Percentile(record_frames, 90), Percentile(record_frames, 95),
         Percentile(record_frames, 99), Percentile(record_frames, 99.9),
         record_frames.empty() ? 0.0f : *std::max_element(record_frames.begin(), record_frames.end()));
    both("frames_over_20ms=%zu\nframes_over_34ms=%zu\n",
         std::size_t(std::count_if(record_frames.begin(), record_frames.end(), [](float v) { return v > 20.0f; })),
         std::size_t(std::count_if(record_frames.begin(), record_frames.end(), [](float v) { return v > 34.0f; })));
    const double timed = record_window_frames;
    both("cpu_translate_ms_per_frame=%.3f\n", timed && BbStats::enabled ? record_cpu_us / 1000.0 / timed : -1.0);
    both("gpu_ms_per_frame=%.3f\n", timed ? record_gpu_ns / 1e6 / timed : -1.0);
    both("gpu_util_avg=%.1f\n", record_gpu_util_samples ? record_gpu_util_sum / record_gpu_util_samples : -1.0);
    both("cpu_util_avg=%.1f\n", record_util_samples ? record_cpu_util_sum / record_util_samples : 0.0);
    both("compiles=%llu\n", static_cast<unsigned long long>(record_compiles));
    both("gpu=%s\n", gpu_name.c_str());
    for (std::size_t i = 0; i < std::min<std::size_t>(threads.size(), 16); ++i) {
        both("thread=%u|%s|%.1f\n", threads[i].second, record_thread_names[threads[i].second].c_str(),
             threads[i].first);
    }
    both("files=%s-*.csv\n", record_base.c_str());
    if (summary) {
        std::fclose(summary);
    }
    std::fclose(frames_csv);
    std::fclose(seconds_csv);
    std::fclose(threads_csv);
    frames_csv = seconds_csv = threads_csv = nullptr;
    recording = false;
}

void SamplerThread() {
    InitGpuUtilization();
    std::map<std::uint32_t, std::pair<std::uint64_t, std::string>> last;
    auto last_time = Clock::now();
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // The measurement also ends on time when no frame is presented: a minimised window
        // skips presentation (vk_presenter), and only OnPresent would close it.
        if (recording && std::chrono::duration<double>(Clock::now() - record_start).count() >= record_seconds + 2) {
            FinishMeasurement();
        }
        if (!Active()) {
            last.clear();
            continue;
        }
        System s;
        SystemCpuAndRam(s);
        s.gpu_util = GpuUtilization();
        if (allocator) {
            VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
            vmaGetHeapBudgets(allocator, budgets);
            double used = 0.0, budget = 0.0;
            for (const VmaBudget& b : budgets) {
                used += double(b.usage);
                budget += double(b.budget);
            }
            s.vram_used_mb = float(used / 1048576.0);
            s.vram_budget_mb = float(budget / 1048576.0);
        }
        std::map<std::uint32_t, std::pair<std::uint64_t, std::string>> now_times;
        ThreadTimes(now_times);
        const auto now = Clock::now();
        const double elapsed_100ns = std::chrono::duration<double>(now - last_time).count() * 1e7;
        last_time = now;
        for (const auto& [tid, value] : now_times) {
            const auto it = last.find(tid);
            if (it != last.end() && value.first >= it->second.first && elapsed_100ns > 0) {
                const float percent = float(double(value.first - it->second.first) / elapsed_100ns * 100.0);
                s.threads.push_back({value.second.empty() ? "tid " + std::to_string(tid) : value.second, tid, percent});
            }
        }
        last = std::move(now_times);
        std::sort(s.threads.begin(), s.threads.end(),
                  [](const ThreadLoad& a, const ThreadLoad& b) { return a.percent > b.percent; });
        if (recording) {
            std::scoped_lock lock{record_mutex};
            if (threads_csv) {
                const double t = std::chrono::duration<double>(now - record_start).count();
                for (const ThreadLoad& load : s.threads) {
                    std::fprintf(threads_csv, "%.2f,%u,\"%s\",%.1f\n", t, load.tid, load.name.c_str(), load.percent);
                    auto& sum = record_thread_sum[load.tid];
                    sum.first += load.percent;
                    ++sum.second;
                    record_thread_names[load.tid] = load.name;
                }
                record_cpu_util_sum += s.cpu_util;
                ++record_util_samples;
                if (s.gpu_util >= 0.0f) {
                    record_gpu_util_sum += s.gpu_util;
                    ++record_gpu_util_samples;
                }
            }
        }
        std::scoped_lock lock{system_mutex};
        system_now = std::move(s);
    }
}

void StartSampler() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(SamplerThread).detach(); });
}

} // namespace

void Init(const Vulkan::Instance& instance) {
    std::scoped_lock lock{gpu_mutex};
    if (query_pool) {
        return;
    }
    device = instance.GetDevice();
    allocator = instance.GetAllocator();
    gpu_name = std::string(instance.GetModelName());
    period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    const auto [pool_result, pool] = device.createQueryPool({.queryType = vk::QueryType::eTimestamp,
                                                             .queryCount = 2 * Slots});
    const auto [command_result, commands] = device.createCommandPool(
        {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
         .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()});
    if (pool_result != vk::Result::eSuccess || command_result != vk::Result::eSuccess) {
        std::printf("Perf: GPU timing unavailable (query pool %d, command pool %d)\n", int(pool_result),
                    int(command_result));
        return;
    }
    const auto [alloc_result, buffers] = device.allocateCommandBuffers(
        {.commandPool = commands, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 2 * Slots});
    if (alloc_result != vk::Result::eSuccess) {
        std::printf("Perf: GPU timing unavailable (command buffers %d)\n", int(alloc_result));
        return;
    }
    for (std::uint32_t i = 0; i < Slots; ++i) {
        slots[i].begin = buffers[2 * i];
        slots[i].end = buffers[2 * i + 1];
    }
    device.resetQueryPool(pool, 0, 2 * Slots);
    query_pool = pool;
    command_pool = commands;
    std::printf("Perf: telemetry ready (%s, timestamp period %.2f ns); HUD with F11 or the in-game menu\n",
                gpu_name.c_str(), period_ns);
    // BB_PERF_MEASURE=<delay s>:<length s>: a measurement that starts by itself (unattended runs).
    if (const char* env = std::getenv("BB_PERF_MEASURE"); env && *env) {
        int delay = 0, length = 0;
        if (std::sscanf(env, "%d:%d", &delay, &length) == 2 && delay >= 0 && length > 0) {
            std::thread([delay, length] {
                std::this_thread::sleep_for(std::chrono::seconds(delay));
                StartMeasurement(length);
            }).detach();
        }
    }
}

std::uint32_t WrapSubmission(vk::CommandBuffer* cmdbufs, std::uint32_t count, std::uint32_t capacity) {
    if (!Active() || count + 2 > capacity) {
        return count;
    }
    std::scoped_lock lock{gpu_mutex};
    if (!query_pool) {
        return count;
    }
    CollectGpu();
    Slot& slot = slots[next_slot];
    if (slot.pending) {
        return count; // the ring is full of unfinished submissions: this one goes untimed
    }
    const std::uint32_t q = 2 * next_slot;
    // Reset from the host (hostQueryReset, as vk_gpu_profiler): until the GPU writes the new
    // timestamps the queries read as unavailable. A reset recorded in the command buffer left the
    // previous values readable as "available" until the GPU ran it, and the slot was reused while
    // its command buffers were still pending.
    device.resetQueryPool(query_pool, q, 2);
    const vk::CommandBufferBeginInfo begin_info{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
    if (slot.begin.reset() != vk::Result::eSuccess || slot.begin.begin(begin_info) != vk::Result::eSuccess) {
        return count;
    }
    slot.begin.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, query_pool, q);
    if (slot.begin.end() != vk::Result::eSuccess || slot.end.reset() != vk::Result::eSuccess ||
        slot.end.begin(begin_info) != vk::Result::eSuccess) {
        return count;
    }
    slot.end.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query_pool, q + 1);
    if (slot.end.end() != vk::Result::eSuccess) {
        return count;
    }
    std::memmove(cmdbufs + 1, cmdbufs, count * sizeof(vk::CommandBuffer));
    cmdbufs[0] = slot.begin;
    cmdbufs[count + 1] = slot.end;
    slot.pending = true;
    pending_slots.push_back(next_slot);
    next_slot = (next_slot + 1) % Slots;
    return count + 2;
}

void SetHudVisible(bool visible) {
    hud_visible = visible;
    if (visible) {
        StartSampler();
    }
}

bool Active() {
    return hud_visible.load(std::memory_order_relaxed) || recording.load(std::memory_order_relaxed);
}

bool StartMeasurement(int seconds) {
    std::scoped_lock lock{record_mutex};
    if (recording) {
        return false;
    }
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
    record_base = LogsDir() + "/perf-" + stamp;
    frames_csv = std::fopen((record_base + "-frames.csv").c_str(), "w");
    seconds_csv = std::fopen((record_base + "-seconds.csv").c_str(), "w");
    threads_csv = std::fopen((record_base + "-threads.csv").c_str(), "w");
    if (!frames_csv || !seconds_csv || !threads_csv) {
        std::printf("Perf: cannot write the measurement files at %s-*.csv\n", record_base.c_str());
        for (FILE* f : {frames_csv, seconds_csv, threads_csv}) {
            if (f) {
                std::fclose(f);
            }
        }
        frames_csv = seconds_csv = threads_csv = nullptr;
        return false;
    }
    std::fprintf(frames_csv, "t_s,frame_ms\n");
    std::fprintf(seconds_csv, "t_s,fps,low1_fps,frame_ms_p50,frame_ms_p99,cpu_translate_ms,gpu_ms,gpu_util,"
                              "cpu_util,vram_mb,compiles\n");
    std::fprintf(threads_csv, "t_s,tid,name,cpu_percent\n");
    record_frames.clear();
    record_frames.reserve(std::size_t(seconds) * 200);
    record_cpu_us = record_gpu_ns = record_compiles = 0;
    record_window_frames = 0;
    restart_window = true; // the first second starts with the measurement
    record_gpu_util_sum = record_cpu_util_sum = 0.0;
    record_util_samples = record_gpu_util_samples = 0;
    record_thread_sum.clear();
    record_thread_names.clear();
    record_seconds = seconds;
    record_start = Clock::now();
    recording = true;
    StartSampler();
    std::printf("Perf: measurement started (%d s) -> %s-*.csv\n", seconds, record_base.c_str());
    return true;
}

std::pair<float, int> MeasurementProgress() {
    if (!recording) {
        return {-1.0f, 0};
    }
    return {float(std::chrono::duration<double>(Clock::now() - record_start).count()), record_seconds};
}

void OnPresent(float frame_ms) {
    if (!Active() || frame_ms <= 0.0f || frame_ms > 2000.0f) {
        return;
    }
    const auto now = Clock::now();
    std::uint64_t busy_now;
    {
        std::scoped_lock lock{gpu_mutex};
        CollectGpu();
        busy_now = gpu_busy_ns;
    }
    std::scoped_lock lock{frame_mutex};
    recent_frames.emplace_back(now, frame_ms);
    while (!recent_frames.empty() && now - recent_frames.front().first > std::chrono::seconds(10)) {
        recent_frames.pop_front();
    }
    history.push_back(frame_ms);
    if (history.size() > HistorySize) {
        history.erase(history.begin());
    }
    if (window.start == Clock::time_point{} || restart_window.exchange(false)) {
        window = {now, 0, CpuTranslateUs(), busy_now, compiles_total.load()};
    }
    ++window.frames;
    if (recording) {
        std::scoped_lock record_lock{record_mutex};
        if (frames_csv) {
            std::fprintf(frames_csv, "%.4f,%.3f\n", std::chrono::duration<double>(now - record_start).count(),
                         frame_ms);
            record_frames.push_back(frame_ms);
        }
    }

    const double seconds = std::chrono::duration<double>(now - window.start).count();
    if (seconds < 1.0) {
        return;
    }
    // A second is complete: the HUD's values and, while recording, a row.
    const std::uint64_t cpu_now = CpuTranslateUs(), compiles_now = compiles_total.load();
    std::vector<float> last10;
    last10.reserve(recent_frames.size());
    std::vector<float> last_second;
    for (const auto& [at, ms] : recent_frames) {
        last10.push_back(ms);
        if (now - at <= std::chrono::duration<double>(seconds)) {
            last_second.push_back(ms);
        }
    }
    compile_marks.emplace_back(now, compiles_now);
    while (compile_marks.size() > 1 && now - compile_marks.front().first > std::chrono::seconds(5)) {
        compile_marks.pop_front();
    }
    Snapshot s;
    s.fps = float(window.frames / seconds);
    s.frame_ms = s.fps > 0.0f ? 1000.0f / s.fps : 0.0f;
    s.low1_fps = LowFps(last10, 0.01);
    s.cpu_translate_ms = BbStats::enabled && cpu_now >= window.cpu_us
                             ? float((cpu_now - window.cpu_us) / 1000.0 / window.frames)
                             : -1.0f;
    s.gpu_ms = busy_now > window.gpu_ns ? float((busy_now - window.gpu_ns) / 1e6 / window.frames) : -1.0f;
    s.compiles_total = compiles_now;
    s.compiles_last_5s = std::uint32_t(compiles_now - compile_marks.front().second);
    s.gpu_name = gpu_name;
    s.frame_history = history;
    {
        std::scoped_lock system_lock{system_mutex};
        s.gpu_util = system_now.gpu_util;
        s.cpu_util = system_now.cpu_util;
        s.ram_used_gb = system_now.ram_used_gb;
        s.ram_total_gb = system_now.ram_total_gb;
        s.vram_used_mb = system_now.vram_used_mb;
        s.vram_budget_mb = system_now.vram_budget_mb;
        s.top_threads.assign(system_now.threads.begin(),
                             system_now.threads.begin() + std::min<std::size_t>(system_now.threads.size(), 3));
    }
    if (recording) {
        std::scoped_lock record_lock{record_mutex};
        if (seconds_csv) {
            std::fprintf(seconds_csv, "%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.1f,%.1f,%.0f,%llu\n",
                         std::chrono::duration<double>(now - record_start).count(), s.fps,
                         LowFps(last_second, 0.01), Percentile(last_second, 50), Percentile(last_second, 99),
                         s.cpu_translate_ms, s.gpu_ms, s.gpu_util, s.cpu_util, s.vram_used_mb,
                         static_cast<unsigned long long>(compiles_now - window.compiles));
            if (cpu_now >= window.cpu_us) {
                record_cpu_us += cpu_now - window.cpu_us;
            }
            record_gpu_ns += busy_now - window.gpu_ns;
            record_compiles += compiles_now - window.compiles;
            record_window_frames += window.frames;
        }
    }
    snapshot = std::move(s);
    window = {now, 0, cpu_now, busy_now, compiles_now};
    if (recording && std::chrono::duration<double>(now - record_start).count() >= record_seconds) {
        FinishMeasurement();
    }
}

Snapshot GetSnapshot() {
    std::scoped_lock lock{frame_mutex};
    return snapshot;
}

} // namespace BbPerf
