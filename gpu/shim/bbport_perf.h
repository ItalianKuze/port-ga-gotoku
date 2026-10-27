// SPDX-License-Identifier: GPL-2.0-or-later
// PortGaGotoku: performance telemetry for the statistics HUD and a repeatable measurement.
//
// Per frame: the present interval (FPS, frame time, 1% low). Per second: the CPU time of the GPU
// command thread (PM4 translation and handing the work to the recording threads; BbStats, needs
// PGG_FRAME_STATS), the GPU's busy time from timestamp queries around each queue submission, GPU
// utilisation (NVML, else Windows PDH), VRAM (VMA budgets), system CPU and RAM, pipeline compiles,
// and the CPU use of every thread of the process. A measurement records all of it for a fixed
// time (F9, the in-game menu, or PGG_PERF_MEASURE=<delay s>:<length s> by itself) into
// PortGaGotoku-data/logs/perf-<date-time>-*.csv and prints a summary (scripts/
// perf_report.py turns it into a report). Self-contained on purpose: the measurement must run the
// same way after the update from upstream, so the renderer is touched in two places only
// (WrapSubmission in the scheduler, OnPresent in the overlay).

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbPerf {

/// Shader/pipeline compiles since start (vk_pipeline_cache.cpp counts them).
inline std::atomic<std::uint64_t> compiles_total{0};

/// Once, with the device (the overlay's Init).
void Init(const Vulkan::Instance& instance);

/// The scheduler, right before a queue submission of `count` command buffers: while telemetry is
/// on, puts a command buffer with a start timestamp first and one with an end timestamp last.
/// Returns the new count (`count` when off or when it does not fit in `capacity`).
std::uint32_t WrapSubmission(vk::CommandBuffer* cmdbufs, std::uint32_t count, std::uint32_t capacity);

/// Every present, with the interval since the previous one.
void OnPresent(float frame_ms);

/// Telemetry runs while the HUD shows or a measurement records.
void SetHudVisible(bool visible);
bool Active();

/// Starts a measurement of `seconds`; false when one is already running.
bool StartMeasurement(int seconds);
/// Seconds recorded so far and the length, or {-1, 0} when none runs.
std::pair<float, int> MeasurementProgress();

struct ThreadLoad {
    std::string name;
    std::uint32_t tid = 0;
    float percent = 0.0f; ///< of one core
};

/// What the HUD shows (the last complete second, the graph and the 1% low of the last 10 s).
struct Snapshot {
    float fps = 0.0f, frame_ms = 0.0f;
    float low1_fps = 0.0f;        ///< average of the slowest 1% of frames in the last 10 s, as FPS
    float cpu_translate_ms = -1;  ///< GPU command thread CPU ms per frame (-1: not measured)
    float gpu_ms = -1;            ///< GPU busy ms per frame (-1: not measured yet)
    float gpu_util = -1;          ///< percent, -1 when unavailable
    float cpu_util = 0.0f;        ///< system, percent
    float ram_used_gb = 0.0f, ram_total_gb = 0.0f;
    float vram_used_mb = 0.0f, vram_budget_mb = 0.0f;
    std::uint64_t compiles_total = 0;
    std::uint32_t compiles_last_5s = 0;
    std::string gpu_name;
    std::vector<ThreadLoad> top_threads; ///< the busiest, highest first
    std::vector<float> frame_history;    ///< oldest first
};
Snapshot GetSnapshot();

} // namespace BbPerf
