#include "scanner_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <streambuf>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif
#endif

namespace fs = std::filesystem;

namespace {

static bool g_debug_enabled = false;

#define DEBUG_LOG(...)                              \
    do {                                            \
        if (g_debug_enabled) {                      \
            std::fprintf(stderr, __VA_ARGS__);      \
        }                                           \
    } while (0)

constexpr uint64_t JAVA_MASK = ((1ULL << 48U) - 1ULL);
constexpr uint64_t JAVA_DOMAIN = (JAVA_MASK + 1ULL);
constexpr uint64_t JAVA_MULT = 25214903917ULL;
constexpr uint64_t JAVA_ADD = 11ULL;
constexpr uint32_t INT32_SIGN = 0x80000000U;
constexpr int64_t REGION_X_MULT = 341873128712LL;
constexpr int64_t REGION_Z_MULT = 132897987541LL;
constexpr int CHUNK_SIZE = 16;
constexpr int BIOME_SAMPLE_STEP_BLOCKS = 4;

static std::string lower_ascii(std::string s);
static std::string trim(const std::string &s);
static std::string normalize_key(std::string s);
static std::string read_text_file_utf8_bom_tolerant(const fs::path &path);
static std::string fmt_seed(uint64_t value);
static std::string fmt_i64(int64_t value);
static std::string fmt_double(double value, int precision);
static uint64_t lift_seed_to_64(uint64_t seed, uint16_t upper16);

struct StructurePreset {
    int spacing = 0;
    int separation = 0;
    int salt = 0;
    int spread_type = SCAN_SPREAD_LINEAR;
};

struct RegionTerm {
    int rx = 0;
    int rz = 0;
    int base_block_x = 0;
    int base_block_z = 0;
    uint64_t add_term_mod48 = 0;
};

struct StructureContext {
    std::string name;
    StructurePreset preset;
    std::vector<RegionTerm> regions;
};

struct ConstraintSpec {
    std::string cid;
    std::string structure;
    std::string validation_structure;
    std::string dimension = "overworld";
    std::string mode;
    std::string anchor;
    int radius = 0;
    uint32_t min_required = 1U;
    uint32_t quad_accuracy = 0U;
    uint32_t viability_flags = 0U;
    bool strict_only_variant = false;
    std::unordered_map<std::string, std::vector<std::string>> structure_settings;
};

struct BiomeFilterSpec {
    std::string point;
    std::string dimension = "overworld";
    int y = 64;
    int radius = 0;
    std::vector<int32_t> allowed_ids;
};

struct LootRequirementSpec {
    std::string item;
    int count = 1;
};

struct LootFilterSpec {
    std::string constraint_id;
    std::string structure;
    std::vector<LootRequirementSpec> required;
};

struct SculptSpec {
    bool enabled = false;
    std::string pattern = "disk";
    std::string edge = "outward";
    std::string center = "origin";
    std::string dimension = "overworld";
    int y = 64;
    int radius = 128;
    int step = 16;
    double dominance_threshold = 0.85;
    std::vector<int32_t> allowed_ids;
};

struct PerfSpec {
    int strict_workers = 0;
    std::string candidate_cap_mode = "none";
    int fixed_cap = 128;
    int spawn_anchor_slack = 1024;
    std::string strict_surrogate = "off";
    std::string execution_mode = "auto";
    std::string compiled_stage_a_mode = "auto";
};

struct SafetySpec {
    std::string completeness = "exact";  // exact|approximate
};

struct QuerySpec {
    int version = 1;
    std::string logic = "and";
    std::string default_anchor = "origin";
    std::string default_dimension = "overworld";
    std::vector<ConstraintSpec> constraints;
    std::vector<BiomeFilterSpec> biome_filters;
    std::vector<LootFilterSpec> loot_filters;
    SculptSpec sculpt;
    PerfSpec perf;
    SafetySpec safety;
};

struct QueryConstraintRuntime {
    ConstraintSpec spec;
    int dimension = SCAN_DIM_OVERWORLD;
    bool prefilter_supported = false;
    bool exact_attempt_prefilter_supported = false;
    bool structure_in_presets = false;
    int struct_id = -1;
    int prefilter_radius = 0;
    int candidate_cap = 0;
    uint64_t candidate_radius_sq = 0;
    uint64_t root_radius_sq = 0;
    uint32_t candidate_chunk_radius = 0;
    uint32_t strict_region_spacing_hint = 1;
    uint32_t min_required = 1U;
    uint32_t quad_max_span = 0U;
    StructurePreset attempt_preset;
    StructureContext context;
};

struct QueryRuntime {
    QuerySpec spec;
    std::vector<std::string> ordered_constraint_ids;
    std::unordered_map<std::string, QueryConstraintRuntime> constraints;
};

struct GpuRegionTerm {
    int32_t base_block_x = 0;
    int32_t base_block_z = 0;
    uint64_t add_term_mod48 = 0;
    uint32_t bound = 0;
    uint32_t constraint_index = 0;
    uint32_t spread_type = 0;
    uint32_t use_fast_next_int = 0;
};

struct GpuConstraintDesc {
    uint32_t region_start = 0;
    uint32_t region_count = 0;
    uint64_t radius_sq = 0;
    int32_t anchor_x = 0;
    int32_t anchor_z = 0;
    uint32_t gate_div = 1;
    uint32_t gate_salt = 0;
    uint32_t is_gate_only = 0;
    uint32_t min_required = 1;
    uint32_t quad_max_span = 0;
};

struct GpuPrefilterPlan {
    std::vector<GpuRegionTerm> regions;
    std::vector<GpuConstraintDesc> constraints;
};

struct GpuPlanConstraintEntry {
    GpuConstraintDesc desc{};
    std::vector<GpuRegionTerm> regions;
    uint32_t original_order = 0U;
    uint32_t anchor_priority = 1U;
};

using FnGpuIsAvailable = int (*)();
using FnGpuTotalMem = uint64_t (*)();
using FnGpuFilterMulti = void (*)(
    uint64_t,
    uint64_t,
    GpuRegionTerm *,
    uint32_t,
    GpuConstraintDesc *,
    uint32_t,
    uint64_t *,
    uint32_t *);
using FnGpuFilterMultiChecked = int (*)(
    uint64_t,
    uint64_t,
    GpuRegionTerm *,
    uint32_t,
    GpuConstraintDesc *,
    uint32_t,
    uint64_t *,
    uint32_t *);
using FnGpuFilterDoubleBufferAvailable = int (*)();
using FnGpuFilterAsyncMaxSlots = uint32_t (*)();
using FnGpuFilterMultiSubmit = int (*)(
    uint32_t,
    uint64_t,
    uint64_t,
    GpuRegionTerm *,
    uint32_t,
    GpuConstraintDesc *,
    uint32_t);
using FnGpuFilterMultiCollect = int (*)(
    uint32_t,
    uint64_t *,
    uint64_t,
    uint32_t *,
    double *,
    double *);

struct ScannerCompiledQueryPlan;
using FnScannerCoreCompileQueryPlan = ScannerCompiledQueryPlan *(*)(
    const NativeRegionTerm *,
    uint32_t,
    const NativeQueryConstraintDesc *,
    uint32_t,
    const NativeBiomeFilterDesc *,
    uint32_t,
    const int32_t *,
    uint32_t);
using FnScannerCoreFreeQueryPlan = void (*)(ScannerCompiledQueryPlan *);
using FnScannerCoreValidateQueryBatchCompiled = int (*)(
    const ScannerCompiledQueryPlan *,
    const uint64_t *,
    uint32_t,
    int32_t,
    uint32_t,
    uint64_t *,
    uint32_t *,
    uint32_t *,
    uint32_t *,
    uint32_t *);
using FnScannerCoreValidateQueryBatchCompiledPostStageA = FnScannerCoreValidateQueryBatchCompiled;
using FnScannerCoreCollectQueryDetailsCompiled = int (*)(
    const ScannerCompiledQueryPlan *,
    uint64_t,
    int32_t,
    NativeQueryMatchOut *,
    uint32_t,
    uint32_t *,
    NativeQueryBiomeOut *,
    uint32_t,
    uint32_t *,
    uint32_t *);
using FnScannerCoreGetCompiledBatchStats = int (*)(const ScannerCompiledQueryPlan *, ScannerCompiledBatchStats *);

#if defined(_WIN32)
using ModuleHandle = HMODULE;
static void *lookup_symbol(ModuleHandle module, const char *name) {
    return reinterpret_cast<void *>(GetProcAddress(module, name));
}
static void close_module(ModuleHandle module) {
    if (module != nullptr) {
        FreeLibrary(module);
    }
}
#else
using ModuleHandle = void *;
static void *lookup_symbol(ModuleHandle module, const char *name) {
    return dlsym(module, name);
}
static void close_module(ModuleHandle module) {
    if (module != nullptr) {
        dlclose(module);
    }
}
#endif

static const char *gpu_filter_library_filename_for(const std::string &backend) {
    if (backend == "opencl") {
#if defined(_WIN32)
        return "gpu_filter_opencl.dll";
#elif defined(__APPLE__)
        return "libgpu_filter_opencl.dylib";
#else
        return "libgpu_filter_opencl.so";
#endif
    }
#if defined(_WIN32)
    return "gpu_filter.dll";
#elif defined(__APPLE__)
    return "libgpu_filter.dylib";
#else
    return "libgpu_filter.so";
#endif
}

static const char *gpu_filter_library_filename() {
    return gpu_filter_library_filename_for("cuda");
}

static const char *cubiomes_library_filename() {
#if defined(_WIN32)
    return "lib.dll";
#elif defined(__APPLE__)
    return "lib.dylib";
#else
    return "lib.so";
#endif
}

static const char *gpu_backend_readme_path_for(const std::string &backend) {
    if (backend == "opencl") {
        return "opencl/README.md";
    }
#if defined(__APPLE__)
    return "macos/README.md";
#else
    return "cuda/BUILD.md";
#endif
}

static const char *gpu_backend_readme_path() {
    return gpu_backend_readme_path_for("cuda");
}

static const char *native_scanner_program_name() {
#if defined(_WIN32)
    return "scanner_native.exe";
#else
    return "scanner_native";
#endif
}

static char java_classpath_separator() {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

static void append_classpath_segment(std::string &classpath, const std::string &segment) {
    const std::string trimmed = trim(segment);
    if (trimmed.empty()) {
        return;
    }
    if (!classpath.empty()) {
        classpath.push_back(java_classpath_separator());
    }
    classpath.append(trimmed);
}

static bool is_loot_validation_protocol_line(const std::string &line) {
    return line.rfind("OK\t", 0) == 0 || line.rfind("ERR\t", 0) == 0;
}

class GpuFilterApi {
public:
    // Compatibility entry point: defaults to "auto" backend selection
    // (try CUDA first, fall back to OpenCL).
    bool load(const fs::path &repo_root) {
        return load(repo_root, "auto");
    }

    // backend = "auto" | "cuda" | "opencl". Returns true if a backend was
    // loaded and exposes the required symbols.
    bool load(const fs::path &repo_root, const std::string &backend) {
        if (loaded_) {
            return ready_;
        }

        std::vector<std::string> backends_to_try;
        if (backend == "opencl") {
            backends_to_try = {"opencl"};
        } else if (backend == "cuda") {
            backends_to_try = {"cuda"};
        } else {
            // auto: try CUDA first (typically faster on NVIDIA hardware),
            // then OpenCL for everything else.
            backends_to_try = {"cuda", "opencl"};
        }

        for (const std::string &b : backends_to_try) {
            if (try_load_backend(repo_root, b)) {
                active_backend_ = b;
                break;
            }
        }

        loaded_ = true;
        ready_ = module_ != nullptr && gpu_is_available_ != nullptr && gpu_total_mem_ != nullptr &&
                 (gpu_filter_multi_checked_ != nullptr || gpu_filter_multi_ != nullptr);
        return ready_;
    }

    const std::string &active_backend() const { return active_backend_; }
    const fs::path &loaded_path() const { return loaded_path_; }

    bool is_available() const {
        return ready_ && gpu_is_available_() == 1;
    }

    uint64_t total_mem_bytes() const {
        if (!ready_) {
            return 0;
        }
        return gpu_total_mem_();
    }

    void filter_multi_into(
        uint64_t start_seed,
        uint64_t count,
        const std::vector<GpuRegionTerm> &regions,
        const std::vector<GpuConstraintDesc> &constraints,
        std::vector<uint64_t> &out
    ) const {
        if (count == 0) {
            out.clear();
            return;
        }
        if (constraints.empty()) {
            out.resize(static_cast<size_t>(count));
            for (uint64_t i = 0; i < count; ++i) {
                out[static_cast<size_t>(i)] = start_seed + i;
            }
            return;
        }
        if (!ready_) {
            throw std::runtime_error("GPU filter backend not loaded.");
        }
        if (count > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error(
                "GPU filter batch exceeds the 32-bit hit-count ABI limit; reduce the batch size.");
        }

        if (gpu_output_capacity_ < count) {
            gpu_output_scratch_.reset(new uint64_t[static_cast<size_t>(count)]);
            gpu_output_capacity_ = count;
        }
        uint32_t hit_count = 0;

        GpuRegionTerm dummy_region{};
        GpuConstraintDesc dummy_constraint{};
        GpuRegionTerm *region_ptr =
            regions.empty() ? &dummy_region : const_cast<GpuRegionTerm *>(regions.data());
        GpuConstraintDesc *constraint_ptr =
            constraints.empty() ? &dummy_constraint : const_cast<GpuConstraintDesc *>(constraints.data());

        if (gpu_filter_multi_checked_ != nullptr) {
            const int status = gpu_filter_multi_checked_(
                start_seed,
                count,
                region_ptr,
                static_cast<uint32_t>(regions.size()),
                constraint_ptr,
                static_cast<uint32_t>(constraints.size()),
                gpu_output_scratch_.get(),
                &hit_count
            );
            if (status != 0) {
                throw std::runtime_error("GPU filter backend failed with status " + std::to_string(status) + ".");
            }
        } else {
            gpu_filter_multi_(
                start_seed,
                count,
                region_ptr,
                static_cast<uint32_t>(regions.size()),
                constraint_ptr,
                static_cast<uint32_t>(constraints.size()),
                gpu_output_scratch_.get(),
                &hit_count
            );
        }
        if (static_cast<uint64_t>(hit_count) > count) {
            throw std::runtime_error("GPU filter backend returned more hits than the submitted batch capacity.");
        }
        out.assign(gpu_output_scratch_.get(), gpu_output_scratch_.get() + hit_count);
    }

    std::vector<uint64_t> filter_multi(
        uint64_t start_seed,
        uint64_t count,
        const std::vector<GpuRegionTerm> &regions,
        const std::vector<GpuConstraintDesc> &constraints
    ) const {
        std::vector<uint64_t> out;
        filter_multi_into(start_seed, count, regions, constraints, out);
        return out;
    }

    // Patch 1: async double-buffered submit/collect.
    //
    // async_available() returns true when the loaded DLL exports the new
    // submit/collect entry points AND its self-check returns 1. Callers that
    // need a fallback should keep using filter_multi_into.
    bool async_available() const {
        if (!ready_) {
            return false;
        }
        if (gpu_filter_multi_submit_ == nullptr || gpu_filter_multi_collect_ == nullptr) {
            return false;
        }
        if (gpu_filter_double_buffer_available_ == nullptr) {
            return false;
        }
        return gpu_filter_double_buffer_available_() != 0;
    }

    uint32_t async_max_slots() const {
        if (!async_available()) {
            return 0U;
        }
        if (gpu_filter_async_max_slots_ == nullptr) {
            return 2U;
        }
        const uint32_t slots = gpu_filter_async_max_slots_();
        if (slots < 2U) {
            return 2U;
        }
        return std::min<uint32_t>(slots, 8U);
    }

    // Submit a Stage A batch on the given slot (0 or 1). Does NOT block.
    // After submit returns, the caller may continue with CPU work; collect
    // the matching slot when ready to consume survivors.
    void submit_batch(
        uint32_t slot_id,
        uint64_t start_seed,
        uint64_t count,
        const std::vector<GpuRegionTerm> &regions,
        const std::vector<GpuConstraintDesc> &constraints
    ) const {
        if (!async_available()) {
            throw std::runtime_error("submit_batch: async API not available.");
        }
        const uint32_t max_slots = async_max_slots();
        if (slot_id >= max_slots) {
            throw std::runtime_error("submit_batch: invalid slot id.");
        }
        if (count > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error(
                "submit_batch: batch exceeds the 32-bit hit-count ABI limit; reduce the batch size.");
        }
        ensure_async_slot_capacity(slot_id);
        async_slot_capacity_[slot_id] = count;
        async_slot_submitted_[slot_id] = count;

        GpuRegionTerm dummy_region{};
        GpuConstraintDesc dummy_constraint{};
        GpuRegionTerm *region_ptr =
            regions.empty() ? &dummy_region : const_cast<GpuRegionTerm *>(regions.data());
        GpuConstraintDesc *constraint_ptr =
            constraints.empty() ? &dummy_constraint : const_cast<GpuConstraintDesc *>(constraints.data());

        const int status = gpu_filter_multi_submit_(
            slot_id,
            start_seed,
            count,
            region_ptr,
            static_cast<uint32_t>(regions.size()),
            constraint_ptr,
            static_cast<uint32_t>(constraints.size())
        );
        if (status != 0) {
            throw std::runtime_error(
                "GPU filter submit failed with status " + std::to_string(status) + ".");
        }
    }

    // Collect the survivors for the given slot into out. Blocks on the slot's
    // GPU events. Reports per-batch GPU kernel and D2H transfer time.
    void collect_batch(
        uint32_t slot_id,
        std::vector<uint64_t> &out,
        double &kernel_seconds,
        double &transfer_seconds
    ) const {
        kernel_seconds = 0.0;
        transfer_seconds = 0.0;
        if (!async_available()) {
            throw std::runtime_error("collect_batch: async API not available.");
        }
        const uint32_t max_slots = async_max_slots();
        if (slot_id >= max_slots) {
            throw std::runtime_error("collect_batch: invalid slot id.");
        }
        ensure_async_slot_capacity(slot_id);
        const uint64_t capacity = async_slot_capacity_[slot_id];
        if (async_slot_scratch_capacity_[slot_id] < capacity) {
            async_slot_scratch_[slot_id].reset(new uint64_t[static_cast<size_t>(capacity == 0ULL ? 1ULL : capacity)]);
            async_slot_scratch_capacity_[slot_id] = capacity;
        }
        uint64_t *buffer = async_slot_scratch_[slot_id].get();
        uint32_t hit_count = 0;
        const int status = gpu_filter_multi_collect_(
            slot_id,
            buffer,
            capacity,
            &hit_count,
            &kernel_seconds,
            &transfer_seconds
        );
        if (status != 0) {
            throw std::runtime_error(
                "GPU filter collect failed with status " + std::to_string(status) + ".");
        }
        if (static_cast<uint64_t>(hit_count) > capacity) {
            throw std::runtime_error("GPU filter collect returned more hits than the submitted batch capacity.");
        }
        out.assign(buffer, buffer + hit_count);
        async_slot_submitted_[slot_id] = 0;
    }

private:
    void ensure_async_slot_capacity(uint32_t slot_id) const {
        const size_t need = static_cast<size_t>(slot_id) + 1U;
        if (async_slot_capacity_.size() < need) {
            async_slot_capacity_.resize(need, 0ULL);
            async_slot_submitted_.resize(need, 0ULL);
            async_slot_scratch_.resize(need);
            async_slot_scratch_capacity_.resize(need, 0ULL);
        }
    }

    bool try_load_backend(const fs::path &repo_root, const std::string &backend) {
        const char *fname = gpu_filter_library_filename_for(backend);
        const char *backend_dir = (backend == "opencl") ? "opencl" : "cuda";
        const char *env_var = (backend == "opencl") ? "GPU_FILTER_OPENCL_DLL" : "GPU_FILTER_DLL";

        std::vector<fs::path> candidates;
        const char *env = std::getenv(env_var);
        if (env != nullptr && *env != '\0') {
            candidates.emplace_back(env);
        }
        // Generic env override applies to whichever backend is being loaded.
        if (backend != "cuda") {
            const char *generic_env = std::getenv("GPU_FILTER_DLL");
            if (generic_env != nullptr && *generic_env != '\0') {
                candidates.emplace_back(generic_env);
            }
        }
        candidates.emplace_back(repo_root / fname);
#if defined(__APPLE__)
        if (backend == "cuda") {
            candidates.emplace_back(repo_root / "macos" / fname);
        }
#endif
        candidates.emplace_back(repo_root / backend_dir / fname);
        candidates.emplace_back(fs::current_path() / fname);
#if defined(__APPLE__)
        if (backend == "cuda") {
            candidates.emplace_back(fs::current_path() / "macos" / fname);
        }
#endif
        candidates.emplace_back(fs::current_path() / backend_dir / fname);

        for (const fs::path &candidate : candidates) {
            if (!fs::exists(candidate)) {
                continue;
            }
            if (try_load(candidate)) {
                return true;
            }
        }
        return false;
    }

    bool try_load(const fs::path &candidate) {
#if defined(_WIN32)
        ModuleHandle module = LoadLibraryA(candidate.string().c_str());
#else
        ModuleHandle module = dlopen(candidate.string().c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
        if (module == nullptr) {
            return false;
        }
        module_ = module;
        loaded_path_ = candidate;
        gpu_is_available_ = reinterpret_cast<FnGpuIsAvailable>(lookup_symbol(module_, "gpu_is_available"));
        gpu_total_mem_ = reinterpret_cast<FnGpuTotalMem>(lookup_symbol(module_, "gpu_total_mem"));
        gpu_filter_multi_ = reinterpret_cast<FnGpuFilterMulti>(lookup_symbol(module_, "gpu_filter_multi"));
        gpu_filter_multi_checked_ =
            reinterpret_cast<FnGpuFilterMultiChecked>(lookup_symbol(module_, "gpu_filter_multi_checked"));
        gpu_filter_double_buffer_available_ = reinterpret_cast<FnGpuFilterDoubleBufferAvailable>(
            lookup_symbol(module_, "gpu_filter_double_buffer_available"));
        gpu_filter_async_max_slots_ = reinterpret_cast<FnGpuFilterAsyncMaxSlots>(
            lookup_symbol(module_, "gpu_filter_async_max_slots"));
        gpu_filter_multi_submit_ = reinterpret_cast<FnGpuFilterMultiSubmit>(
            lookup_symbol(module_, "gpu_filter_multi_submit"));
        gpu_filter_multi_collect_ = reinterpret_cast<FnGpuFilterMultiCollect>(
            lookup_symbol(module_, "gpu_filter_multi_collect"));
        const bool required_symbols =
            gpu_is_available_ != nullptr &&
            gpu_total_mem_ != nullptr &&
            (gpu_filter_multi_checked_ != nullptr || gpu_filter_multi_ != nullptr);
        if (!required_symbols || gpu_is_available_() != 1) {
            close_module(module_);
            module_ = nullptr;
            loaded_path_.clear();
            gpu_is_available_ = nullptr;
            gpu_total_mem_ = nullptr;
            gpu_filter_multi_ = nullptr;
            gpu_filter_multi_checked_ = nullptr;
            gpu_filter_double_buffer_available_ = nullptr;
            gpu_filter_async_max_slots_ = nullptr;
            gpu_filter_multi_submit_ = nullptr;
            gpu_filter_multi_collect_ = nullptr;
            return false;
        }
        return true;
    }

    std::string active_backend_;
    fs::path loaded_path_;
    ModuleHandle module_ = nullptr;
    FnGpuIsAvailable gpu_is_available_ = nullptr;
    FnGpuTotalMem gpu_total_mem_ = nullptr;
    FnGpuFilterMulti gpu_filter_multi_ = nullptr;
    FnGpuFilterMultiChecked gpu_filter_multi_checked_ = nullptr;
    FnGpuFilterDoubleBufferAvailable gpu_filter_double_buffer_available_ = nullptr;
    FnGpuFilterAsyncMaxSlots gpu_filter_async_max_slots_ = nullptr;
    FnGpuFilterMultiSubmit gpu_filter_multi_submit_ = nullptr;
    FnGpuFilterMultiCollect gpu_filter_multi_collect_ = nullptr;
    mutable std::unique_ptr<uint64_t[]> gpu_output_scratch_;
    mutable uint64_t gpu_output_capacity_ = 0ULL;
    mutable std::vector<std::unique_ptr<uint64_t[]>> async_slot_scratch_;
    mutable std::vector<uint64_t> async_slot_scratch_capacity_;
    mutable std::vector<uint64_t> async_slot_capacity_;
    mutable std::vector<uint64_t> async_slot_submitted_;
    bool loaded_ = false;
    bool ready_ = false;
};

class ScannerCoreQueryPlanApi {
public:
    bool load() {
        if (loaded_) {
            return ready_;
        }

#if defined(_WIN32)
        ModuleHandle module = GetModuleHandleA("scanner_core.dll");
        if (module == nullptr) {
            module = GetModuleHandleA("scanner_core_new.dll");
        }
#else
        ModuleHandle module = reinterpret_cast<ModuleHandle>(RTLD_DEFAULT);
#endif

        if (module != nullptr) {
            compile_query_plan_ =
                reinterpret_cast<FnScannerCoreCompileQueryPlan>(lookup_symbol(module, "scanner_core_compile_query_plan"));
            free_query_plan_ =
                reinterpret_cast<FnScannerCoreFreeQueryPlan>(lookup_symbol(module, "scanner_core_free_query_plan"));
            validate_query_batch_compiled_ = reinterpret_cast<FnScannerCoreValidateQueryBatchCompiled>(
                lookup_symbol(module, "scanner_core_validate_query_batch_compiled"));
            validate_query_batch_compiled_post_stage_a_ =
                reinterpret_cast<FnScannerCoreValidateQueryBatchCompiledPostStageA>(
                    lookup_symbol(module, "scanner_core_validate_query_batch_compiled_post_stage_a"));
            collect_query_details_compiled_ = reinterpret_cast<FnScannerCoreCollectQueryDetailsCompiled>(
                lookup_symbol(module, "scanner_core_collect_query_details_compiled"));
            get_compiled_batch_stats_ = reinterpret_cast<FnScannerCoreGetCompiledBatchStats>(
                lookup_symbol(module, "scanner_core_get_compiled_batch_stats"));
        }

        loaded_ = true;
        ready_ = compile_query_plan_ != nullptr && free_query_plan_ != nullptr &&
                 validate_query_batch_compiled_ != nullptr && collect_query_details_compiled_ != nullptr;
        return ready_;
    }

    bool ready() const {
        return ready_;
    }

    bool has_compiled_batch_stats_api() const {
        return get_compiled_batch_stats_ != nullptr;
    }

    ScannerCompiledQueryPlan *compile_query_plan(
        const NativeRegionTerm *regions,
        uint32_t region_count,
        const NativeQueryConstraintDesc *constraints,
        uint32_t constraint_count,
        const NativeBiomeFilterDesc *biome_filters,
        uint32_t biome_filter_count,
        const int32_t *biome_allowed_ids,
        uint32_t biome_allowed_count
    ) const {
        if (!ready_) {
            return nullptr;
        }
        return compile_query_plan_(
            regions,
            region_count,
            constraints,
            constraint_count,
            biome_filters,
            biome_filter_count,
            biome_allowed_ids,
            biome_allowed_count
        );
    }

    void free_query_plan(ScannerCompiledQueryPlan *plan) const {
        if (plan != nullptr && ready_ && free_query_plan_ != nullptr) {
            free_query_plan_(plan);
        }
    }

    int validate_query_batch_compiled(
        const ScannerCompiledQueryPlan *plan,
        const uint64_t *seeds,
        uint32_t seed_count,
        int32_t cubi_mc_version,
        uint32_t worker_count,
        uint64_t *valid_out,
        uint32_t *valid_count,
        uint32_t *mismatch_count,
        uint32_t *biome_reject_count,
        uint32_t *cap_pruned_total
    ) const {
        if (!ready_ || plan == nullptr) {
            return -1;
        }
        return validate_query_batch_compiled_(
            plan,
            seeds,
            seed_count,
            cubi_mc_version,
            worker_count,
            valid_out,
            valid_count,
            mismatch_count,
            biome_reject_count,
            cap_pruned_total
        );
    }

    int validate_query_batch_compiled_post_stage_a(
        const ScannerCompiledQueryPlan *plan,
        const uint64_t *seeds,
        uint32_t seed_count,
        int32_t cubi_mc_version,
        uint32_t worker_count,
        uint64_t *valid_out,
        uint32_t *valid_count,
        uint32_t *mismatch_count,
        uint32_t *biome_reject_count,
        uint32_t *cap_pruned_total
    ) const {
        if (!ready_ || plan == nullptr || validate_query_batch_compiled_post_stage_a_ == nullptr) {
            return validate_query_batch_compiled(
                plan,
                seeds,
                seed_count,
                cubi_mc_version,
                worker_count,
                valid_out,
                valid_count,
                mismatch_count,
                biome_reject_count,
                cap_pruned_total
            );
        }
        return validate_query_batch_compiled_post_stage_a_(
            plan,
            seeds,
            seed_count,
            cubi_mc_version,
            worker_count,
            valid_out,
            valid_count,
            mismatch_count,
            biome_reject_count,
            cap_pruned_total
        );
    }

    int collect_query_details_compiled(
        const ScannerCompiledQueryPlan *plan,
        uint64_t seed,
        int32_t cubi_mc_version,
        NativeQueryMatchOut *match_out,
        uint32_t match_cap,
        uint32_t *match_count,
        NativeQueryBiomeOut *biome_out,
        uint32_t biome_cap,
        uint32_t *biome_count,
        uint32_t *cap_pruned_total
    ) const {
        if (!ready_ || plan == nullptr) {
            return -1;
        }
        return collect_query_details_compiled_(
            plan,
            seed,
            cubi_mc_version,
            match_out,
            match_cap,
            match_count,
            biome_out,
            biome_cap,
            biome_count,
            cap_pruned_total
        );
    }

    int get_compiled_batch_stats(const ScannerCompiledQueryPlan *plan, ScannerCompiledBatchStats *stats) const {
        if (!ready_ || plan == nullptr || stats == nullptr || get_compiled_batch_stats_ == nullptr) {
            return -1;
        }
        return get_compiled_batch_stats_(plan, stats);
    }

private:
    bool loaded_ = false;
    bool ready_ = false;
    FnScannerCoreCompileQueryPlan compile_query_plan_ = nullptr;
    FnScannerCoreFreeQueryPlan free_query_plan_ = nullptr;
    FnScannerCoreValidateQueryBatchCompiled validate_query_batch_compiled_ = nullptr;
    FnScannerCoreValidateQueryBatchCompiledPostStageA validate_query_batch_compiled_post_stage_a_ = nullptr;
    FnScannerCoreCollectQueryDetailsCompiled collect_query_details_compiled_ = nullptr;
    FnScannerCoreGetCompiledBatchStats get_compiled_batch_stats_ = nullptr;
};

static ScannerCoreQueryPlanApi &scanner_core_query_plan_api() {
    static ScannerCoreQueryPlanApi api;
    api.load();
    return api;
}

struct CompiledQueryPlanContext {
    std::vector<NativeRegionTerm> regions;
    std::vector<NativeQueryConstraintDesc> constraints;
    std::vector<NativeBiomeFilterDesc> biome_filters;
    std::vector<int32_t> biome_allowed_ids;
    int32_t cubi_mc_version = 0;
    ScannerCompiledQueryPlan *compiled_plan = nullptr;

    CompiledQueryPlanContext() = default;
    CompiledQueryPlanContext(const CompiledQueryPlanContext &) = delete;
    CompiledQueryPlanContext &operator=(const CompiledQueryPlanContext &) = delete;

    ~CompiledQueryPlanContext() {
        reset();
    }

    void reset() {
        if (compiled_plan != nullptr) {
            scanner_core_query_plan_api().free_query_plan(compiled_plan);
            compiled_plan = nullptr;
        }
    }

    bool compiled_ready() const {
        return compiled_plan != nullptr && scanner_core_query_plan_api().ready();
    }

    void build(
        std::vector<NativeRegionTerm> native_regions,
        std::vector<NativeQueryConstraintDesc> native_constraints,
        std::vector<NativeBiomeFilterDesc> native_biome_filters,
        std::vector<int32_t> native_biome_allowed_ids,
        int32_t mc_version
    ) {
        reset();
        regions = std::move(native_regions);
        constraints = std::move(native_constraints);
        biome_filters = std::move(native_biome_filters);
        biome_allowed_ids = std::move(native_biome_allowed_ids);
        cubi_mc_version = mc_version;
        compiled_plan = scanner_core_query_plan_api().compile_query_plan(
            regions.empty() ? nullptr : regions.data(),
            static_cast<uint32_t>(regions.size()),
            constraints.empty() ? nullptr : constraints.data(),
            static_cast<uint32_t>(constraints.size()),
            biome_filters.empty() ? nullptr : biome_filters.data(),
            static_cast<uint32_t>(biome_filters.size()),
            biome_allowed_ids.empty() ? nullptr : biome_allowed_ids.data(),
            static_cast<uint32_t>(biome_allowed_ids.size())
        );
    }

    int validate_batch(
        const uint64_t *seeds,
        uint32_t seed_count,
        uint32_t worker_count,
        uint64_t *valid_out,
        uint32_t *valid_count,
        uint32_t *mismatch_count,
        uint32_t *biome_reject_count,
        uint32_t *cap_pruned_total
    ) const {
        if (compiled_plan != nullptr && scanner_core_query_plan_api().ready()) {
            return scanner_core_query_plan_api().validate_query_batch_compiled(
                compiled_plan,
                seeds,
                seed_count,
                cubi_mc_version,
                worker_count,
                valid_out,
                valid_count,
                mismatch_count,
                biome_reject_count,
                cap_pruned_total
            );
        }
        return scanner_core_validate_query_batch(
            seeds,
            seed_count,
            regions.empty() ? nullptr : regions.data(),
            static_cast<uint32_t>(regions.size()),
            constraints.empty() ? nullptr : constraints.data(),
            static_cast<uint32_t>(constraints.size()),
            biome_filters.empty() ? nullptr : biome_filters.data(),
            static_cast<uint32_t>(biome_filters.size()),
            biome_allowed_ids.empty() ? nullptr : biome_allowed_ids.data(),
            static_cast<uint32_t>(biome_allowed_ids.size()),
            cubi_mc_version,
            worker_count,
            valid_out,
            valid_count,
            mismatch_count,
            biome_reject_count,
            cap_pruned_total
        );
    }

    int validate_batch_compiled(
        const uint64_t *seeds,
        uint32_t seed_count,
        uint32_t worker_count,
        uint64_t *valid_out,
        uint32_t *valid_count,
        uint32_t *mismatch_count,
        uint32_t *biome_reject_count,
        uint32_t *cap_pruned_total
    ) const {
        if (!compiled_ready()) {
            return -1;
        }
        return scanner_core_query_plan_api().validate_query_batch_compiled(
            compiled_plan,
            seeds,
            seed_count,
            cubi_mc_version,
            worker_count,
            valid_out,
            valid_count,
            mismatch_count,
            biome_reject_count,
            cap_pruned_total
        );
    }

    int validate_batch_post_stage_a(
        const uint64_t *seeds,
        uint32_t seed_count,
        uint32_t worker_count,
        uint64_t *valid_out,
        uint32_t *valid_count,
        uint32_t *mismatch_count,
        uint32_t *biome_reject_count,
        uint32_t *cap_pruned_total
    ) const {
        if (compiled_plan != nullptr && scanner_core_query_plan_api().ready()) {
            return scanner_core_query_plan_api().validate_query_batch_compiled_post_stage_a(
                compiled_plan,
                seeds,
                seed_count,
                cubi_mc_version,
                worker_count,
                valid_out,
                valid_count,
                mismatch_count,
                biome_reject_count,
                cap_pruned_total
            );
        }
        return validate_batch(
            seeds,
            seed_count,
            worker_count,
            valid_out,
            valid_count,
            mismatch_count,
            biome_reject_count,
            cap_pruned_total
        );
    }

    int validate_batch_legacy(
        const uint64_t *seeds,
        uint32_t seed_count,
        uint32_t worker_count,
        uint64_t *valid_out,
        uint32_t *valid_count,
        uint32_t *mismatch_count,
        uint32_t *biome_reject_count,
        uint32_t *cap_pruned_total
    ) const {
        return scanner_core_validate_query_batch_legacy(
            seeds,
            seed_count,
            regions.empty() ? nullptr : regions.data(),
            static_cast<uint32_t>(regions.size()),
            constraints.empty() ? nullptr : constraints.data(),
            static_cast<uint32_t>(constraints.size()),
            biome_filters.empty() ? nullptr : biome_filters.data(),
            static_cast<uint32_t>(biome_filters.size()),
            biome_allowed_ids.empty() ? nullptr : biome_allowed_ids.data(),
            static_cast<uint32_t>(biome_allowed_ids.size()),
            cubi_mc_version,
            worker_count,
            valid_out,
            valid_count,
            mismatch_count,
            biome_reject_count,
            cap_pruned_total
        );
    }

    int collect_details(
        uint64_t seed,
        NativeQueryMatchOut *match_out,
        uint32_t match_cap,
        uint32_t *match_count,
        NativeQueryBiomeOut *biome_out,
        uint32_t biome_cap,
        uint32_t *biome_count,
        uint32_t *cap_pruned_total,
        bool use_biome_filters
    ) const {
        if (use_biome_filters && compiled_plan != nullptr && scanner_core_query_plan_api().ready()) {
            return scanner_core_query_plan_api().collect_query_details_compiled(
                compiled_plan,
                seed,
                cubi_mc_version,
                match_out,
                match_cap,
                match_count,
                biome_out,
                biome_cap,
                biome_count,
                cap_pruned_total
            );
        }
        return scanner_core_collect_query_details(
            seed,
            regions.empty() ? nullptr : regions.data(),
            static_cast<uint32_t>(regions.size()),
            constraints.empty() ? nullptr : constraints.data(),
            static_cast<uint32_t>(constraints.size()),
            use_biome_filters && !biome_filters.empty() ? biome_filters.data() : nullptr,
            use_biome_filters ? static_cast<uint32_t>(biome_filters.size()) : 0U,
            use_biome_filters && !biome_allowed_ids.empty() ? biome_allowed_ids.data() : nullptr,
            use_biome_filters ? static_cast<uint32_t>(biome_allowed_ids.size()) : 0U,
            cubi_mc_version,
            match_out,
            match_cap,
            match_count,
            biome_out,
            biome_cap,
            biome_count,
            cap_pruned_total
        );
    }

    int collect_details_compiled(
        uint64_t seed,
        NativeQueryMatchOut *match_out,
        uint32_t match_cap,
        uint32_t *match_count,
        NativeQueryBiomeOut *biome_out,
        uint32_t biome_cap,
        uint32_t *biome_count,
        uint32_t *cap_pruned_total
    ) const {
        if (!compiled_ready()) {
            return -1;
        }
        return scanner_core_query_plan_api().collect_query_details_compiled(
            compiled_plan,
            seed,
            cubi_mc_version,
            match_out,
            match_cap,
            match_count,
            biome_out,
            biome_cap,
            biome_count,
            cap_pruned_total
        );
    }

    int collect_details_legacy(
        uint64_t seed,
        NativeQueryMatchOut *match_out,
        uint32_t match_cap,
        uint32_t *match_count,
        NativeQueryBiomeOut *biome_out,
        uint32_t biome_cap,
            uint32_t *biome_count,
            uint32_t *cap_pruned_total,
            bool use_biome_filters
    ) const {
        return scanner_core_collect_query_details_legacy(
            seed,
            regions.empty() ? nullptr : regions.data(),
            static_cast<uint32_t>(regions.size()),
            constraints.empty() ? nullptr : constraints.data(),
            static_cast<uint32_t>(constraints.size()),
            use_biome_filters && !biome_filters.empty() ? biome_filters.data() : nullptr,
            use_biome_filters ? static_cast<uint32_t>(biome_filters.size()) : 0U,
            use_biome_filters && !biome_allowed_ids.empty() ? biome_allowed_ids.data() : nullptr,
            use_biome_filters ? static_cast<uint32_t>(biome_allowed_ids.size()) : 0U,
            cubi_mc_version,
            match_out,
            match_cap,
            match_count,
            biome_out,
            biome_cap,
            biome_count,
            cap_pruned_total
        );
    }
};

static std::string normalize_loot_item_id(std::string s) {
    s = lower_ascii(trim(std::move(s)));
    if (s.rfind("minecraft:", 0) == 0) {
        s = s.substr(std::strlen("minecraft:"));
    }
    return s;
}

static std::string normalize_loot_structure_id(std::string s) {
    s = lower_ascii(trim(std::move(s)));
    if (s.rfind("minecraft:", 0) == 0) {
        s = s.substr(std::strlen("minecraft:"));
    }
    for (char &c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (s.rfind("data/minecraft/loot_table/", 0) == 0) {
        s = s.substr(std::strlen("data/minecraft/loot_table/"));
    }
    if (s.rfind("loot_table/", 0) == 0) {
        s = s.substr(std::strlen("loot_table/"));
    }
    if (s.rfind("chests/", 0) == 0) {
        s = s.substr(std::strlen("chests/"));
    }
    if (s.size() > 5U && s.substr(s.size() - 5U) == ".json") {
        s.resize(s.size() - 5U);
    }
    for (char &c : s) {
        if (c == '/' || c == '-' || c == ' ') {
            c = '_';
        }
    }
    if (s == "desert_temple") {
        return "desert_pyramid";
    }
    if (s == "jungle_pyramid") {
        return "jungle_temple";
    }
    if (s == "mansion") {
        return "woodland_mansion";
    }
    if (s == "blacksmith" || s == "weaponsmith" || s == "village_blacksmith") {
        return "village_weaponsmith";
    }
    if (s == "nether_bridge" || s == "nether_fortress") {
        return s == "nether_bridge" ? "nether_bridge" : "fortress";
    }
    if (s == "bastion") {
        return "bastion_remnant";
    }
    if (s == "underwater_ruin") {
        return "ocean_ruin";
    }
    return s;
}

static bool is_shipwreck_loot_structure_id(const std::string &structure_id) {
    const std::string s = normalize_loot_structure_id(structure_id);
    return s == "shipwreck" || s == "shipwreck_supply" || s == "shipwreck_map" || s == "shipwreck_treasure";
}

static bool is_ruined_portal_loot_structure_id(const std::string &structure_id) {
    const std::string s = normalize_loot_structure_id(structure_id);
    return s == "ruined_portal" || s == "ruined_portal_desert" || s == "ruined_portal_jungle" ||
           s == "ruined_portal_mountain" || s == "ruined_portal_nether" || s == "ruined_portal_ocean" ||
           s == "ruined_portal_swamp";
}

static bool is_exact_village_candidate_loot_structure_id(const std::string &structure_id) {
    const std::string s = normalize_loot_structure_id(structure_id);
    return s == "village_weaponsmith";
}

static std::string loot_family_for_structure_id(const std::string &structure_id) {
    const std::string s = normalize_loot_structure_id(structure_id);
    if (s == "abandoned_mineshaft") {
        return "mineshaft";
    }
    if (s == "ancient_city" || s == "ancient_city_ice_box") {
        return "ancient_city";
    }
    if (s == "bastion_bridge" || s == "bastion_hoglin_stable" || s == "bastion_other" ||
        s == "bastion_treasure" || s == "bastion_remnant") {
        return "bastion_remnant";
    }
    if (s == "end_city_treasure" || s == "end_city") {
        return "end_city";
    }
    if (s == "nether_bridge" || s == "fortress") {
        return "fortress";
    }
    if (s == "igloo_chest" || s == "igloo") {
        return "igloo";
    }
    if (s == "jungle_temple" || s == "jungle_temple_dispenser") {
        return "jungle_temple";
    }
    if (is_ruined_portal_loot_structure_id(s)) {
        return "ruined_portal";
    }
    if (s == "underwater_ruin_big" || s == "underwater_ruin_small" || s == "ocean_ruin") {
        return "ocean_ruin";
    }
    if (s == "shipwreck" || s == "shipwreck_supply" || s == "shipwreck_map" || s == "shipwreck_treasure") {
        return "shipwreck";
    }
    if (s == "stronghold" || s == "stronghold_corridor" || s == "stronghold_crossing" ||
        s == "stronghold_library") {
        return "stronghold";
    }
    if (s.rfind("trial_chambers", 0) == 0) {
        return "trial_chambers";
    }
    if (s.rfind("village_", 0) == 0 || s == "village") {
        return "village";
    }
    return s;
}

static bool loot_source_matches_constraint(const std::string &source, const std::string &constraint_structure) {
    return loot_family_for_structure_id(source) == loot_family_for_structure_id(constraint_structure);
}

static std::string exact_shipwreck_roll_structure_id(const std::string &structure_id, bool is_beached) {
    const std::string s = normalize_loot_structure_id(structure_id);
    const char *suffix = is_beached ? "_beached" : "_ocean";
    if (s == "shipwreck") {
        return std::string("shipwreck") + suffix;
    }
    if (s == "shipwreck_supply") {
        return std::string("shipwreck_supply") + suffix;
    }
    if (s == "shipwreck_map") {
        return std::string("shipwreck_map") + suffix;
    }
    if (s == "shipwreck_treasure") {
        return std::string("shipwreck_treasure") + suffix;
    }
    return s;
}

static bool is_supported_loot_version_token(const std::string &version_token) {
    const std::string token = trim(version_token);
    return token == "26.1.2" || token == "26.1.1" || token == "1.21.11" || token == "1.17" || token == "1.17.1";
}

static bool is_exact_loot_version_token(const std::string &version_token) {
    const std::string token = trim(version_token);
    return token == "26.1.2" || token == "26.1.1" || token == "1.21.11";
}

static bool is_supported_loot_structure_for_version(
    const std::string &version_token,
    const std::string &structure_id
) {
    const std::string structure = normalize_loot_structure_id(structure_id);
    if (is_exact_loot_version_token(version_token)) {
        static const std::unordered_set<std::string> supported = {
            "buried_treasure",
            "ruined_portal",
            "ruined_portal_desert",
            "ruined_portal_jungle",
            "ruined_portal_mountain",
            "ruined_portal_nether",
            "ruined_portal_ocean",
            "ruined_portal_swamp",
            "village_weaponsmith",
        };
        return supported.find(structure) != supported.end();
    }
    return structure == "ruined_portal" || structure == "buried_treasure" || structure == "desert_pyramid" ||
           structure == "shipwreck";
}

static bool query_uses_exact_shipwreck_validation(const QuerySpec &spec, const std::string &version_token) {
    if (!is_exact_loot_version_token(version_token)) {
        return false;
    }
    for (const ConstraintSpec &constraint : spec.constraints) {
        if (constraint.structure == "shipwreck") {
            return true;
        }
    }
    for (const LootFilterSpec &loot : spec.loot_filters) {
        if (is_shipwreck_loot_structure_id(loot.structure)) {
            return true;
        }
    }
    return false;
}

struct PreparedLootRequirement {
    std::string item;
    int count = 1;
};

struct PreparedLootFilter {
    uint32_t constraint_index = 0U;
    std::string constraint_id;
    std::string query_structure;
    std::string effective_structure;
    std::vector<PreparedLootRequirement> required;
    std::string encoded_required;
};

enum class StructureValidationOutcome {
    valid,
    invalid,
    skipped
};

struct PreparedJavaStructureValidationConstraint {
    uint32_t constraint_index = 0U;
    std::string constraint_id;
    std::string structure;
    uint32_t min_required = 1U;
};

struct LootChestDetail {
    std::string table_id;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::unordered_map<std::string, int> counts;
};

struct LootRollDetail {
    std::unordered_map<std::string, int> aggregate_counts;
    std::vector<LootChestDetail> chests;
};

struct LootCheckRequest {
    std::string structure;
    uint64_t seed = 0ULL;
    int32_t block_x = 0;
    int32_t block_z = 0;
    std::string encoded_requirements;
};

struct StructureSettingsRequest {
    std::string structure;
    uint64_t seed = 0ULL;
    int32_t block_x = 0;
    int32_t block_z = 0;
    std::string encoded_settings;
};

struct PreparedStructureSettingsFilter {
    uint32_t constraint_index = 0U;
    std::string constraint_id;
    std::string structure;
    std::string encoded_settings;
};

static bool loot_counts_satisfy_requirements(
    const std::unordered_map<std::string, int> &counts,
    const std::vector<PreparedLootRequirement> &required
) {
    for (const PreparedLootRequirement &req : required) {
        const auto it = counts.find(req.item);
        if (it == counts.end() || it->second < req.count) {
            return false;
        }
    }
    return true;
}

static std::string encode_loot_requirements_for_server(const std::vector<PreparedLootRequirement> &required) {
    if (required.empty()) {
        return "_";
    }
    std::ostringstream oss;
    bool first = true;
    for (const PreparedLootRequirement &req : required) {
        if (req.item.empty() || req.count <= 0) {
            continue;
        }
        if (!first) {
            oss << ';';
        }
        first = false;
        oss << req.item << ':' << req.count;
    }
    return first ? std::string("_") : oss.str();
}

static bool detect_exact_shipwreck_beached(
    uint64_t seed,
    int32_t cubi_mc_version,
    int32_t structure_x,
    int32_t structure_z
) {
    // Shipwreck start positions are chunk-min block coords. Mojang's shipwreck
    // biome selection uses the chunk-center biome; sampling that exact point
    // with cubiomes lets us choose the correct beached/ocean shipwreck variant
    // without trusting the older FeatureUtils biome source.
    const int32_t kBeachBiomeIds[] = {16, 26}; // beach, snowy_beach
    NativeQueryConstraintDesc dummy_constraint{};
    NativeBiomeFilterDesc biome_filter{};
    biome_filter.point_type = SCAN_ANCHOR_FIXED;
    biome_filter.point_dep_index = -1;
    biome_filter.y = 0;
    biome_filter.radius = 0;
    biome_filter.sample_step = 1U;
    biome_filter.allowed_start = 0U;
    biome_filter.allowed_count = 2U;
    biome_filter.point_x = structure_x + 8;
    biome_filter.point_z = structure_z + 8;

    uint32_t match_count = 0U;
    uint32_t biome_count = 0U;
    uint32_t cap_pruned = 0U;
    NativeQueryBiomeOut biome_out{};
    const int rc = scanner_core_collect_query_details(
        seed,
        nullptr,
        0U,
        &dummy_constraint,
        0U,
        &biome_filter,
        1U,
        kBeachBiomeIds,
        2U,
        cubi_mc_version,
        nullptr,
        0U,
        &match_count,
        &biome_out,
        1U,
        &biome_count,
        &cap_pruned
    );
    if (rc < 0) {
        throw std::runtime_error(
            std::string("scanner_core_collect_query_details failed while resolving exact shipwreck biome with code ") +
            std::to_string(rc)
        );
    }
    return rc == 0 && biome_count > 0U;
}

static bool parse_loot_count_map(
    const std::string &raw,
    std::unordered_map<std::string, int> &out_counts,
    std::string &error
) {
    out_counts.clear();
    const std::string text = trim(raw);
    if (text.empty() || text == "_") {
        return true;
    }
    std::stringstream ss(text);
    std::string entry;
    while (std::getline(ss, entry, ';')) {
        entry = trim(entry);
        if (entry.empty() || entry == "_") {
            continue;
        }
        const size_t pos = entry.rfind(':');
        if (pos == std::string::npos || pos == 0U || pos + 1U >= entry.size()) {
            error = "Invalid loot count entry: " + entry;
            return false;
        }
        const std::string item = normalize_loot_item_id(entry.substr(0, pos));
        if (item.empty()) {
            error = "Invalid loot count item: " + entry;
            return false;
        }
        int count = 0;
        try {
            count = std::stoi(trim(entry.substr(pos + 1U)));
        } catch (const std::exception &) {
            error = "Invalid loot count value: " + entry;
            return false;
        }
        out_counts[item] += count;
    }
    return true;
}

static bool parse_loot_chest_detail_list(
    const std::string &raw,
    std::vector<LootChestDetail> &out_chests,
    std::string &error
) {
    out_chests.clear();
    const std::string text = trim(raw);
    if (text.empty() || text == "_") {
        return true;
    }

    std::stringstream ss(text);
    std::string entry;
    while (std::getline(ss, entry, '|')) {
        entry = trim(entry);
        if (entry.empty() || entry == "_") {
            continue;
        }

        const size_t first_at = entry.find('@');
        const size_t second_at = first_at == std::string::npos ? std::string::npos : entry.find('@', first_at + 1U);
        if (first_at == std::string::npos || second_at == std::string::npos) {
            error = "Invalid loot chest detail entry: " + entry;
            return false;
        }

        LootChestDetail chest{};
        chest.table_id = trim(entry.substr(0, first_at));
        if (chest.table_id.empty()) {
            error = "Invalid loot chest table id in entry: " + entry;
            return false;
        }

        const std::string coord_text = trim(entry.substr(first_at + 1U, second_at - first_at - 1U));
        std::stringstream coord_ss(coord_text);
        std::string coord_part;
        std::vector<std::string> coords;
        while (std::getline(coord_ss, coord_part, ',')) {
            coords.push_back(trim(coord_part));
        }
        if (coords.size() != 3U) {
            error = "Invalid loot chest coordinates in entry: " + entry;
            return false;
        }
        try {
            chest.x = static_cast<int32_t>(std::stoll(coords[0], nullptr, 0));
            chest.y = static_cast<int32_t>(std::stoll(coords[1], nullptr, 0));
            chest.z = static_cast<int32_t>(std::stoll(coords[2], nullptr, 0));
        } catch (const std::exception &) {
            error = "Invalid loot chest coordinates in entry: " + entry;
            return false;
        }

        if (!parse_loot_count_map(entry.substr(second_at + 1U), chest.counts, error)) {
            return false;
        }
        out_chests.push_back(std::move(chest));
    }
    return true;
}

static std::string format_loot_count_map(const std::unordered_map<std::string, int> &counts) {
    if (counts.empty()) {
        return "_";
    }
    std::vector<std::pair<std::string, int>> items;
    items.reserve(counts.size());
    for (const auto &kv : counts) {
        items.push_back(kv);
    }
    std::sort(items.begin(), items.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
    });

    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0U) {
            oss << "; ";
        }
        oss << items[i].first << ':' << items[i].second;
    }
    return oss.str();
}

static bool parse_loot_roll_detail_response(
    const std::string &response,
    LootRollDetail &out_detail,
    std::string &error
) {
    std::vector<std::string> parts;
    std::stringstream ss(response);
    std::string part;
    while (std::getline(ss, part, '\t')) {
        parts.push_back(part);
    }
    if (parts.size() != 4U || parts[0] != "OK" || parts[1] != "ROLL_DETAIL") {
        error = "Unexpected LootValidationServer response: " + response;
        return false;
    }
    if (!parse_loot_count_map(parts[2], out_detail.aggregate_counts, error)) {
        return false;
    }
    if (!parse_loot_chest_detail_list(parts[3], out_detail.chests, error)) {
        return false;
    }
    return true;
}

#if defined(_WIN32)
static std::string quote_windows_arg(const std::string &arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out;
    out.push_back('"');
    unsigned backslashes = 0U;
    for (char ch : arg) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            out.append(backslashes * 2U + 1U, '\\');
            out.push_back('"');
            backslashes = 0U;
            continue;
        }
        if (backslashes > 0U) {
            out.append(backslashes, '\\');
            backslashes = 0U;
        }
        out.push_back(ch);
    }
    if (backslashes > 0U) {
        out.append(backslashes * 2U, '\\');
    }
    out.push_back('"');
    return out;
}

static int parse_java_major_version(const std::string &version_output) {
    std::smatch match;
    static const std::regex kQuotedVersion(R"JAVA(version\s+"([^"]+)")JAVA", std::regex::icase);
    static const std::regex kOpenJdkVersion(R"((?:openjdk|java)\s+([0-9][0-9._]*))", std::regex::icase);
    std::string token;
    if (std::regex_search(version_output, match, kQuotedVersion) && match.size() >= 2) {
        token = match[1].str();
    } else if (std::regex_search(version_output, match, kOpenJdkVersion) && match.size() >= 2) {
        token = match[1].str();
    } else {
        return 0;
    }
    token = trim(token);
    if (token.empty()) {
        return 0;
    }
    if (token.rfind("1.", 0) == 0) {
        token = token.substr(2);
    }
    const size_t dot = token.find_first_of("._-+");
    if (dot != std::string::npos) {
        token = token.substr(0, dot);
    }
    try {
        return std::stoi(token);
    } catch (const std::exception &) {
        return 0;
    }
}

static int probe_java_major_version(const fs::path &java_path) {
    if (java_path.empty()) {
        return 0;
    }
    const std::string command =
        quote_windows_arg(java_path.string()) + " -version 2>&1";
    FILE *pipe = _popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return 0;
    }
    std::string output;
    char buffer[256];
    while (fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output.append(buffer);
    }
    _pclose(pipe);
    return parse_java_major_version(output);
}

static fs::path find_compatible_java_exe(const fs::path &repo_root) {
    std::vector<fs::path> candidates;

    if (const char *java_home = std::getenv("JAVA_HOME")) {
        const fs::path candidate = fs::path(java_home) / "bin" / "java.exe";
        if (fs::exists(candidate)) {
            candidates.push_back(candidate);
        }
    }

    if (const char *user_profile = std::getenv("USERPROFILE")) {
        const fs::path downloads = fs::path(user_profile) / "Downloads";
        if (fs::exists(downloads)) {
            for (const auto &entry : fs::directory_iterator(downloads, fs::directory_options::skip_permission_denied)) {
                if (!entry.is_directory()) {
                    continue;
                }
                const fs::path direct_candidate = entry.path() / "bin" / "java.exe";
                if (fs::exists(direct_candidate)) {
                    candidates.push_back(direct_candidate);
                }
                for (const auto &subdir : fs::directory_iterator(entry.path(), fs::directory_options::skip_permission_denied)) {
                    if (!subdir.is_directory()) {
                        continue;
                    }
                    const fs::path nested_candidate = subdir.path() / "bin" / "java.exe";
                    if (fs::exists(nested_candidate)) {
                        candidates.push_back(nested_candidate);
                    }
                }
            }
        }
    }

    candidates.push_back(fs::path("java"));

    for (const fs::path &candidate : candidates) {
        const int major = probe_java_major_version(candidate);
        if (major >= 16) {
            return candidate;
        }
    }
    return fs::path("java");
}

static std::string discover_prism_minecraft_2612_classpath_windows() {
    std::vector<fs::path> prism_roots;
    if (const char *appdata = std::getenv("APPDATA")) {
        prism_roots.emplace_back(fs::path(appdata) / "PrismLauncher");
    }
    if (const char *user_profile = std::getenv("USERPROFILE")) {
        prism_roots.emplace_back(fs::path(user_profile) / "AppData" / "Roaming" / "PrismLauncher");
    }
    std::vector<fs::path> jars;
    std::unordered_set<std::string> seen;
    auto append_jar = [&](const fs::path &path) {
        std::error_code ec;
        const fs::path normalized = fs::weakly_canonical(path, ec);
        const fs::path key_path = ec ? path.lexically_normal() : normalized;
        const std::string key = lower_ascii(key_path.string());
        if (seen.insert(key).second) {
            jars.push_back(key_path);
        }
    };

    for (const fs::path &prism_root : prism_roots) {
        const fs::path libraries_root = prism_root / "libraries";
        std::error_code ec;
        if (!fs::exists(libraries_root, ec)) {
            continue;
        }

        const fs::path minecraft_client_2612 =
            libraries_root / "com" / "mojang" / "minecraft" / "26.1.2" / "minecraft-26.1.2-client.jar";
        if (fs::is_regular_file(minecraft_client_2612, ec)) {
            append_jar(minecraft_client_2612);
        }
        const fs::path minecraft_client_2611 =
            libraries_root / "com" / "mojang" / "minecraft" / "26.1.1" / "minecraft-26.1.1-client.jar";
        if (fs::is_regular_file(minecraft_client_2611, ec)) {
            append_jar(minecraft_client_2611);
        }

        fs::recursive_directory_iterator it(
            libraries_root,
            fs::directory_options::skip_permission_denied,
            ec
        );
        const fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            const fs::path path = it->path();
            if (it->is_regular_file(ec) && lower_ascii(path.extension().string()) == ".jar") {
                append_jar(path);
            }
            it.increment(ec);
        }
    }

    std::sort(jars.begin(), jars.end());
    std::string classpath;
    for (const fs::path &jar : jars) {
        append_classpath_segment(classpath, jar.string());
    }
    return classpath;
}

static std::string minecraft_2612_classpath_extension_windows() {
    if (const char *env = std::getenv("HELIXSEED_MINECRAFT_2612_CLASSPATH")) {
        if (*env != '\0') {
            return trim(env);
        }
    }
    return discover_prism_minecraft_2612_classpath_windows();
}

class LootValidationServerClient {
public:
    ~LootValidationServerClient() {
        close();
    }

    bool start(const fs::path &repo_root, std::string &error) {
        close();

        const fs::path project_root = repo_root / "GPULootSeedFinder";
        const fs::path class_root = project_root / "target" / "classes";
        const fs::path class_marker = class_root / "GPULootSeedFinder" / "LootValidationServer.class";
        const fs::path runtime_cp_path = project_root / "target" / "runtime-classpath.txt";
        if (!fs::exists(class_marker) || !fs::exists(runtime_cp_path)) {
            error =
                "Loot filters require compiled GPULootSeedFinder classes. Build "
                "GPULootSeedFinder/target/classes and target/runtime-classpath.txt first.";
            return false;
        }

        std::string runtime_cp = read_text_file_utf8_bom_tolerant(runtime_cp_path.string());
        runtime_cp = trim(runtime_cp);
        if (runtime_cp.empty()) {
            error = "LootValidationServer classpath file is empty: " + runtime_cp_path.string();
            return false;
        }

        fs::path java_path = find_compatible_java_exe(repo_root);

        std::string full_classpath = class_root.string();
        append_classpath_segment(full_classpath, runtime_cp);
        append_classpath_segment(full_classpath, minecraft_2612_classpath_extension_windows());
        std::string command_line =
            quote_windows_arg(java_path.string()) + " -cp " + quote_windows_arg(full_classpath) +
            " GPULootSeedFinder.LootValidationServer";

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE child_stdout_read = nullptr;
        HANDLE child_stdout_write = nullptr;
        HANDLE child_stdin_read = nullptr;
        HANDLE child_stdin_write = nullptr;

        if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) {
            error = "CreatePipe(stdout) failed.";
            return false;
        }
        if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(child_stdout_read);
            CloseHandle(child_stdout_write);
            error = "SetHandleInformation(stdout) failed.";
            return false;
        }
        if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0)) {
            CloseHandle(child_stdout_read);
            CloseHandle(child_stdout_write);
            error = "CreatePipe(stdin) failed.";
            return false;
        }
        if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(child_stdout_read);
            CloseHandle(child_stdout_write);
            CloseHandle(child_stdin_read);
            CloseHandle(child_stdin_write);
            error = "SetHandleInformation(stdin) failed.";
            return false;
        }

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = child_stdin_read;
        si.hStdOutput = child_stdout_write;
        si.hStdError = child_stdout_write;

        PROCESS_INFORMATION pi{};
        std::vector<char> mutable_cmd(command_line.begin(), command_line.end());
        mutable_cmd.push_back('\0');

        const BOOL ok = CreateProcessA(
            nullptr,
            mutable_cmd.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            project_root.string().c_str(),
            &si,
            &pi
        );

        CloseHandle(child_stdin_read);
        CloseHandle(child_stdout_write);

        if (!ok) {
            CloseHandle(child_stdout_read);
            CloseHandle(child_stdin_write);
            error = "Failed to start LootValidationServer with java.";
            return false;
        }

        proc_ = pi;
        stdout_read_ = child_stdout_read;
        stdin_write_ = child_stdin_write;

        std::string ready_line;
        if (!read_line(ready_line)) {
            error = "LootValidationServer exited before sending READY.";
            close();
            return false;
        }
        ready_line = trim(ready_line);
        if (ready_line != "READY") {
            error = "LootValidationServer failed to initialize: " + ready_line;
            close();
            return false;
        }
        return true;
    }

    bool roll(
        const std::string &structure,
        const std::string &version_token,
        uint64_t seed,
        int32_t block_x,
        int32_t block_z,
        std::unordered_map<std::string, int> &out_counts,
        std::string &error
    ) {
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "ROLL\t" << structure << '\t' << trim(version_token) << '\t' << seed << '\t' << block_x << '\t'
            << block_z << '\n';
        const std::string cmd_text = cmd.str();
        DWORD written = 0;
        if (!WriteFile(stdin_write_, cmd_text.data(), static_cast<DWORD>(cmd_text.size()), &written, nullptr) ||
            written != cmd_text.size()) {
            error = "Failed to send ROLL command to LootValidationServer.";
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for ROLL response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() != 4U || parts[0] != "OK" || parts[1] != "ROLL") {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        return parse_loot_count_map(parts[3], out_counts, error);
    }

    bool roll_detail(
        const std::string &structure,
        const std::string &version_token,
        uint64_t seed,
        int32_t block_x,
        int32_t block_z,
        LootRollDetail &out_detail,
        std::string &error
    ) {
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "ROLL_DETAIL\t" << structure << '\t' << trim(version_token) << '\t' << seed << '\t' << block_x << '\t'
            << block_z << '\n';
        const std::string cmd_text = cmd.str();
        DWORD written = 0;
        if (!WriteFile(stdin_write_, cmd_text.data(), static_cast<DWORD>(cmd_text.size()), &written, nullptr) ||
            written != cmd_text.size()) {
            error = "Failed to send ROLL_DETAIL command to LootValidationServer.";
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for ROLL_DETAIL response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        return parse_loot_roll_detail_response(response, out_detail, error);
    }

    bool check(
        const std::string &structure,
        const std::string &version_token,
        uint64_t seed,
        int32_t block_x,
        int32_t block_z,
        const std::string &encoded_requirements,
        bool &out_passed,
        std::string &error
    ) {
        out_passed = false;
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "CHECK\t" << structure << '\t' << trim(version_token) << '\t' << seed << '\t' << block_x << '\t'
            << block_z << '\t' << encoded_requirements << '\n';
        const std::string cmd_text = cmd.str();
        DWORD written = 0;
        if (!WriteFile(stdin_write_, cmd_text.data(), static_cast<DWORD>(cmd_text.size()), &written, nullptr) ||
            written != cmd_text.size()) {
            error = "Failed to send CHECK command to LootValidationServer.";
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for CHECK response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() != 3U || parts[0] != "OK" || parts[1] != "CHECK") {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        const std::string token = lower_ascii(trim(parts[2]));
        if (token == "1" || token == "true" || token == "yes" || token == "pass") {
            out_passed = true;
            return true;
        }
        if (token == "0" || token == "false" || token == "no" || token == "reject") {
            out_passed = false;
            return true;
        }
        error = "Unexpected LootValidationServer CHECK token: " + parts[2];
        return false;
    }

    bool check_many(
        const std::string &version_token,
        const std::vector<LootCheckRequest> &requests,
        std::vector<bool> &out_passed,
        std::string &error
    ) {
        out_passed.assign(requests.size(), false);
        if (requests.empty()) {
            return true;
        }
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "CHECK_BATCH\t" << trim(version_token) << '\t' << requests.size();
        for (const LootCheckRequest &req : requests) {
            cmd << '\t' << req.structure << '\t' << req.seed << '\t' << req.block_x << '\t' << req.block_z
                << '\t' << req.encoded_requirements;
        }
        cmd << '\n';
        const std::string cmd_text = cmd.str();
        DWORD written = 0;
        if (!WriteFile(stdin_write_, cmd_text.data(), static_cast<DWORD>(cmd_text.size()), &written, nullptr) ||
            written != cmd_text.size()) {
            error = "Failed to send CHECK_BATCH command to LootValidationServer.";
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for CHECK_BATCH response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() != 3U || parts[0] != "OK" || parts[1] != "CHECK_BATCH") {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        const std::string bits = trim(parts[2]);
        if (bits.size() != requests.size()) {
            error = "LootValidationServer CHECK_BATCH response length mismatch.";
            return false;
        }
        for (size_t i = 0; i < bits.size(); ++i) {
            if (bits[i] == '1') {
                out_passed[i] = true;
            } else if (bits[i] == '0') {
                out_passed[i] = false;
            } else {
                error = "Unexpected LootValidationServer CHECK_BATCH token.";
                return false;
            }
        }
        return true;
    }

    bool settings_many(
        const std::string &version_token,
        const std::vector<StructureSettingsRequest> &requests,
        std::vector<bool> &out_passed,
        std::string &error
    ) {
        out_passed.assign(requests.size(), false);
        if (requests.empty()) {
            return true;
        }
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "SETTINGS_BATCH\t" << trim(version_token) << '\t' << requests.size();
        for (const StructureSettingsRequest &req : requests) {
            cmd << '\t' << req.structure << '\t' << req.seed << '\t' << req.block_x << '\t' << req.block_z
                << '\t' << req.encoded_settings;
        }
        cmd << '\n';
        const std::string cmd_text = cmd.str();
        DWORD written = 0;
        if (!WriteFile(stdin_write_, cmd_text.data(), static_cast<DWORD>(cmd_text.size()), &written, nullptr) ||
            written != cmd_text.size()) {
            error = "Failed to send SETTINGS_BATCH command to LootValidationServer.";
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for SETTINGS_BATCH response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() != 3U || parts[0] != "OK" || parts[1] != "SETTINGS_BATCH") {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        const std::string bits = trim(parts[2]);
        if (bits.size() != requests.size()) {
            error = "LootValidationServer SETTINGS_BATCH response length mismatch.";
            return false;
        }
        for (size_t i = 0; i < bits.size(); ++i) {
            if (bits[i] == '1') {
                out_passed[i] = true;
            } else if (bits[i] == '0') {
                out_passed[i] = false;
            } else {
                error = "Unexpected LootValidationServer SETTINGS_BATCH token.";
                return false;
            }
        }
        return true;
    }

    bool structure_valid(
        const std::string &structure,
        const std::string &version_token,
        uint64_t seed,
        int32_t block_x,
        int32_t block_z,
        StructureValidationOutcome &outcome,
        std::string &error
    ) {
        outcome = StructureValidationOutcome::skipped;
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "STRUCTURE\t" << structure << '\t' << trim(version_token) << '\t' << seed << '\t' << block_x << '\t'
            << block_z << '\n';
        const std::string cmd_text = cmd.str();
        DWORD written = 0;
        if (!WriteFile(stdin_write_, cmd_text.data(), static_cast<DWORD>(cmd_text.size()), &written, nullptr) ||
            written != cmd_text.size()) {
            error = "Failed to send STRUCTURE command to LootValidationServer.";
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for STRUCTURE response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            const std::string low = lower_ascii(response.substr(4));
            if (low.find("unsupported") != std::string::npos || low.find("invalid command") != std::string::npos ||
                low.find("expected:") != std::string::npos) {
                outcome = StructureValidationOutcome::skipped;
                return true;
            }
            error = response.substr(4);
            return false;
        }

        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (!parts.empty() && parts[0] == "SKIP") {
            outcome = StructureValidationOutcome::skipped;
            return true;
        }
        if (parts.size() < 3U || parts[0] != "OK" ||
            (parts[1] != "STRUCTURE" && parts[1] != "STRUCTURE_VALID")) {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        const std::string valid_token = lower_ascii(trim(parts.back()));
        if (valid_token == "1" || valid_token == "true" || valid_token == "valid" || valid_token == "yes" ||
            valid_token == "pass") {
            outcome = StructureValidationOutcome::valid;
            return true;
        }
        if (valid_token == "0" || valid_token == "false" || valid_token == "invalid" || valid_token == "no" ||
            valid_token == "reject") {
            outcome = StructureValidationOutcome::invalid;
            return true;
        }
        if (valid_token == "skip" || valid_token == "skipped" || valid_token == "unsupported") {
            outcome = StructureValidationOutcome::skipped;
            return true;
        }
        error = "Unexpected LootValidationServer STRUCTURE validity token: " + parts.back();
        return false;
    }

    void close() {
        if (stdin_write_ != nullptr) {
            const char quit_cmd[] = "QUIT\n";
            DWORD written = 0;
            WriteFile(stdin_write_, quit_cmd, sizeof(quit_cmd) - 1U, &written, nullptr);
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
        if (stdout_read_ != nullptr) {
            CloseHandle(stdout_read_);
            stdout_read_ = nullptr;
        }
        if (proc_.hProcess != nullptr) {
            WaitForSingleObject(proc_.hProcess, 250);
            DWORD code = STILL_ACTIVE;
            if (GetExitCodeProcess(proc_.hProcess, &code) && code == STILL_ACTIVE) {
                TerminateProcess(proc_.hProcess, 1U);
                WaitForSingleObject(proc_.hProcess, 250);
            }
            CloseHandle(proc_.hProcess);
            proc_.hProcess = nullptr;
        }
        if (proc_.hThread != nullptr) {
            CloseHandle(proc_.hThread);
            proc_.hThread = nullptr;
        }
        proc_.dwProcessId = 0U;
        proc_.dwThreadId = 0U;
    }

private:
    bool is_running() const {
        return stdin_write_ != nullptr && stdout_read_ != nullptr && proc_.hProcess != nullptr;
    }

    bool read_line(std::string &out_line) {
        out_line.clear();
        if (stdout_read_ == nullptr) {
            return false;
        }
        char ch = '\0';
        DWORD read = 0;
        while (true) {
            if (!ReadFile(stdout_read_, &ch, 1U, &read, nullptr) || read == 0U) {
                return !out_line.empty();
            }
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                return true;
            }
            out_line.push_back(ch);
        }
    }

    bool read_protocol_response(std::string &out_line, std::string &error) {
        std::string line;
        while (read_line(line)) {
            line = trim(line);
            if (line.empty()) {
                continue;
            }
            if (is_loot_validation_protocol_line(line)) {
                out_line = line;
                return true;
            }
        }
        error = "LootValidationServer closed before sending a protocol response.";
        return false;
    }

    HANDLE stdout_read_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    PROCESS_INFORMATION proc_{};
};
#else
static std::string quote_shell_arg(const std::string &arg) {
    if (arg.empty()) {
        return "''";
    }
    if (arg.find_first_of(" \t\n'\"\\$`!&|;()<>{}[]*?~") == std::string::npos) {
        return arg;
    }
    std::string out;
    out.push_back('\'');
    for (char ch : arg) {
        if (ch == '\'') {
            out += "'\\''";
            continue;
        }
        out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

static int parse_java_major_version_linux(const std::string &version_output) {
    std::smatch match;
    static const std::regex kQuotedVersion(R"JAVA(version\s+"([^"]+)")JAVA", std::regex::icase);
    static const std::regex kOpenJdkVersion(R"((?:openjdk|java)\s+([0-9][0-9._]*))", std::regex::icase);
    std::string token;
    if (std::regex_search(version_output, match, kQuotedVersion) && match.size() >= 2) {
        token = match[1].str();
    } else if (std::regex_search(version_output, match, kOpenJdkVersion) && match.size() >= 2) {
        token = match[1].str();
    } else {
        return 0;
    }
    token = trim(token);
    if (token.empty()) {
        return 0;
    }
    if (token.rfind("1.", 0) == 0) {
        token = token.substr(2);
    }
    const size_t dot = token.find_first_of("._-+");
    if (dot != std::string::npos) {
        token = token.substr(0, dot);
    }
    try {
        return std::stoi(token);
    } catch (const std::exception &) {
        return 0;
    }
}

static int probe_java_major_version_linux(const fs::path &java_path) {
    if (java_path.empty()) {
        return 0;
    }
    const std::string command = quote_shell_arg(java_path.string()) + " -version 2>&1";
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return 0;
    }
    std::string output;
    char buffer[256];
    while (fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output.append(buffer);
    }
    pclose(pipe);
    return parse_java_major_version_linux(output);
}

static fs::path find_compatible_java_exe_linux() {
    std::vector<fs::path> candidates;
    if (const char *java_home = std::getenv("JAVA_HOME")) {
        const fs::path candidate = fs::path(java_home) / "bin" / "java";
        if (fs::exists(candidate)) {
            candidates.push_back(candidate);
        }
    }
    candidates.push_back("/usr/bin/java");
    candidates.push_back("/usr/local/bin/java");
    candidates.push_back("java");
    for (const fs::path &candidate : candidates) {
        if (probe_java_major_version_linux(candidate) >= 16) {
            return candidate;
        }
    }
    return fs::path("java");
}

static std::string minecraft_2612_classpath_extension_linux() {
    if (const char *env = std::getenv("HELIXSEED_MINECRAFT_2612_CLASSPATH")) {
        if (*env != '\0') {
            return trim(env);
        }
    }
    return std::string();
}

class LootValidationServerClient {
public:
    ~LootValidationServerClient() {
        close();
    }

    bool start(const fs::path &repo_root, std::string &error) {
        close();

        const fs::path project_root = repo_root / "GPULootSeedFinder";
        const fs::path class_root = project_root / "target" / "classes";
        const fs::path class_marker = class_root / "GPULootSeedFinder" / "LootValidationServer.class";
        const fs::path runtime_cp_path = project_root / "target" / "runtime-classpath.txt";
        if (!fs::exists(class_marker) || !fs::exists(runtime_cp_path)) {
            error =
                "Loot filters require compiled GPULootSeedFinder classes. Build "
                "GPULootSeedFinder/target/classes and target/runtime-classpath.txt first.";
            return false;
        }

        std::string runtime_cp = read_text_file_utf8_bom_tolerant(runtime_cp_path);
        runtime_cp = trim(runtime_cp);
        if (runtime_cp.empty()) {
            error = "LootValidationServer classpath file is empty: " + runtime_cp_path.string();
            return false;
        }

        const fs::path java_path = find_compatible_java_exe_linux();
        std::string full_classpath = class_root.string();
        append_classpath_segment(full_classpath, runtime_cp);
        append_classpath_segment(full_classpath, minecraft_2612_classpath_extension_linux());

        int stdout_pipe[2] = {-1, -1};
        int stdin_pipe[2] = {-1, -1};
        if (::pipe(stdout_pipe) != 0) {
            error = "pipe(stdout) failed.";
            return false;
        }
        if (::pipe(stdin_pipe) != 0) {
            close_fd(stdout_pipe[0]);
            close_fd(stdout_pipe[1]);
            error = "pipe(stdin) failed.";
            return false;
        }

        const pid_t child = ::fork();
        if (child < 0) {
            close_fd(stdout_pipe[0]);
            close_fd(stdout_pipe[1]);
            close_fd(stdin_pipe[0]);
            close_fd(stdin_pipe[1]);
            error = "fork() failed while starting LootValidationServer.";
            return false;
        }
        if (child == 0) {
            const std::string java_arg = java_path.string();
            const std::string project_root_str = project_root.string();
            const std::string main_class = "GPULootSeedFinder.LootValidationServer";
            (void)::dup2(stdin_pipe[0], STDIN_FILENO);
            (void)::dup2(stdout_pipe[1], STDOUT_FILENO);
            (void)::dup2(stdout_pipe[1], STDERR_FILENO);
            close_fd(stdout_pipe[0]);
            close_fd(stdout_pipe[1]);
            close_fd(stdin_pipe[0]);
            close_fd(stdin_pipe[1]);
            (void)::chdir(project_root_str.c_str());
            ::execlp(
                java_arg.c_str(),
                java_arg.c_str(),
                "-cp",
                full_classpath.c_str(),
                main_class.c_str(),
                static_cast<char *>(nullptr)
            );
            _exit(127);
        }

        close_fd(stdin_pipe[0]);
        close_fd(stdout_pipe[1]);
        stdin_write_fd_ = stdin_pipe[1];
        stdout_read_fd_ = stdout_pipe[0];
        child_pid_ = child;

        std::string ready_line;
        if (!read_line(ready_line)) {
            error = "LootValidationServer exited before sending READY.";
            close();
            return false;
        }
        ready_line = trim(ready_line);
        if (ready_line != "READY") {
            error = "LootValidationServer failed to initialize: " + ready_line;
            close();
            return false;
        }
        return true;
    }
    bool roll(
        const std::string &structure,
        const std::string &version_token,
        uint64_t seed,
        int32_t block_x,
        int32_t block_z,
        std::unordered_map<std::string, int> &out_counts,
        std::string &error
    ) {
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "ROLL\t" << structure << '\t' << trim(version_token) << '\t' << seed << '\t' << block_x << '\t'
            << block_z << '\n';
        if (!write_all(stdin_write_fd_, cmd.str(), error)) {
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for ROLL response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() != 4U || parts[0] != "OK" || parts[1] != "ROLL") {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        return parse_loot_count_map(parts[3], out_counts, error);
    }
    bool roll_detail(
        const std::string &structure,
        const std::string &version_token,
        uint64_t seed,
        int32_t block_x,
        int32_t block_z,
        LootRollDetail &out_detail,
        std::string &error
    ) {
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "ROLL_DETAIL\t" << structure << '\t' << trim(version_token) << '\t' << seed << '\t' << block_x << '\t'
            << block_z << '\n';
        if (!write_all(stdin_write_fd_, cmd.str(), error)) {
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for ROLL_DETAIL response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        return parse_loot_roll_detail_response(response, out_detail, error);
    }

    bool check(
        const std::string &structure,
        const std::string &version_token,
        uint64_t seed,
        int32_t block_x,
        int32_t block_z,
        const std::string &encoded_requirements,
        bool &out_passed,
        std::string &error
    ) {
        out_passed = false;
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "CHECK\t" << structure << '\t' << trim(version_token) << '\t' << seed << '\t' << block_x << '\t'
            << block_z << '\t' << encoded_requirements << '\n';
        if (!write_all(stdin_write_fd_, cmd.str(), error)) {
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for CHECK response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() != 3U || parts[0] != "OK" || parts[1] != "CHECK") {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        const std::string token = lower_ascii(trim(parts[2]));
        if (token == "1" || token == "true" || token == "yes" || token == "pass") {
            out_passed = true;
            return true;
        }
        if (token == "0" || token == "false" || token == "no" || token == "reject") {
            out_passed = false;
            return true;
        }
        error = "Unexpected LootValidationServer CHECK token: " + parts[2];
        return false;
    }

    bool check_many(
        const std::string &version_token,
        const std::vector<LootCheckRequest> &requests,
        std::vector<bool> &out_passed,
        std::string &error
    ) {
        out_passed.assign(requests.size(), false);
        if (requests.empty()) {
            return true;
        }
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "CHECK_BATCH\t" << trim(version_token) << '\t' << requests.size();
        for (const LootCheckRequest &req : requests) {
            cmd << '\t' << req.structure << '\t' << req.seed << '\t' << req.block_x << '\t' << req.block_z
                << '\t' << req.encoded_requirements;
        }
        cmd << '\n';
        if (!write_all(stdin_write_fd_, cmd.str(), error)) {
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for CHECK_BATCH response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            error = response.substr(4);
            return false;
        }
        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() != 3U || parts[0] != "OK" || parts[1] != "CHECK_BATCH") {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        const std::string bits = trim(parts[2]);
        if (bits.size() != requests.size()) {
            error = "LootValidationServer CHECK_BATCH response length mismatch.";
            return false;
        }
        for (size_t i = 0; i < bits.size(); ++i) {
            if (bits[i] == '1') {
                out_passed[i] = true;
            } else if (bits[i] == '0') {
                out_passed[i] = false;
            } else {
                error = "Unexpected LootValidationServer CHECK_BATCH token.";
                return false;
            }
        }
        return true;
    }

    bool structure_valid(
        const std::string &structure,
        const std::string &version_token,
        uint64_t seed,
        int32_t block_x,
        int32_t block_z,
        StructureValidationOutcome &outcome,
        std::string &error
    ) {
        outcome = StructureValidationOutcome::skipped;
        if (!is_running()) {
            error = "LootValidationServer is not running.";
            return false;
        }

        std::ostringstream cmd;
        cmd << "STRUCTURE\t" << structure << '\t' << trim(version_token) << '\t' << seed << '\t' << block_x << '\t'
            << block_z << '\n';
        if (!write_all(stdin_write_fd_, cmd.str(), error)) {
            return false;
        }

        std::string response;
        if (!read_protocol_response(response, error)) {
            error = "LootValidationServer closed unexpectedly while waiting for STRUCTURE response.";
            return false;
        }
        if (response.rfind("ERR\t", 0) == 0) {
            const std::string low = lower_ascii(response.substr(4));
            if (low.find("unsupported") != std::string::npos || low.find("invalid command") != std::string::npos ||
                low.find("expected:") != std::string::npos) {
                outcome = StructureValidationOutcome::skipped;
                return true;
            }
            error = response.substr(4);
            return false;
        }

        std::vector<std::string> parts;
        std::stringstream ss(response);
        std::string part;
        while (std::getline(ss, part, '\t')) {
            parts.push_back(part);
        }
        if (!parts.empty() && parts[0] == "SKIP") {
            outcome = StructureValidationOutcome::skipped;
            return true;
        }
        if (parts.size() < 3U || parts[0] != "OK" ||
            (parts[1] != "STRUCTURE" && parts[1] != "STRUCTURE_VALID")) {
            error = "Unexpected LootValidationServer response: " + response;
            return false;
        }
        const std::string valid_token = lower_ascii(trim(parts.back()));
        if (valid_token == "1" || valid_token == "true" || valid_token == "valid" || valid_token == "yes" ||
            valid_token == "pass") {
            outcome = StructureValidationOutcome::valid;
            return true;
        }
        if (valid_token == "0" || valid_token == "false" || valid_token == "invalid" || valid_token == "no" ||
            valid_token == "reject") {
            outcome = StructureValidationOutcome::invalid;
            return true;
        }
        if (valid_token == "skip" || valid_token == "skipped" || valid_token == "unsupported") {
            outcome = StructureValidationOutcome::skipped;
            return true;
        }
        error = "Unexpected LootValidationServer STRUCTURE validity token: " + parts.back();
        return false;
    }
    void close() {
        if (stdin_write_fd_ >= 0) {
            std::string ignored;
            (void)write_all(stdin_write_fd_, "QUIT\n", ignored);
            close_fd(stdin_write_fd_);
        }
        if (stdout_read_fd_ >= 0) {
            close_fd(stdout_read_fd_);
        }
        if (child_pid_ > 0) {
            int status = 0;
            const pid_t waited = ::waitpid(child_pid_, &status, WNOHANG);
            if (waited == 0) {
                (void)::kill(child_pid_, SIGTERM);
                for (int i = 0; i < 10; ++i) {
                    if (::waitpid(child_pid_, &status, WNOHANG) == child_pid_) {
                        child_pid_ = -1;
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                (void)::kill(child_pid_, SIGKILL);
                (void)::waitpid(child_pid_, &status, 0);
            } else if (waited < 0 && errno != ECHILD) {
                (void)::kill(child_pid_, SIGKILL);
                (void)::waitpid(child_pid_, &status, 0);
            }
            child_pid_ = -1;
        }
    }

private:
    static void close_fd(int &fd) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    bool is_running() const {
        return stdin_write_fd_ >= 0 && stdout_read_fd_ >= 0 && child_pid_ > 0;
    }

    bool write_all(int fd, const std::string &text, std::string &error) {
        size_t offset = 0;
        while (offset < text.size()) {
            const ssize_t written = ::write(fd, text.data() + offset, text.size() - offset);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error = "Failed to write to LootValidationServer.";
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    bool read_line(std::string &out_line) {
        out_line.clear();
        if (stdout_read_fd_ < 0) {
            return false;
        }
        char ch = '\0';
        while (true) {
            const ssize_t read_count = ::read(stdout_read_fd_, &ch, 1U);
            if (read_count == 0) {
                return !out_line.empty();
            }
            if (read_count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return !out_line.empty();
            }
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                return true;
            }
            out_line.push_back(ch);
        }
    }

    bool read_protocol_response(std::string &out_line, std::string &error) {
        std::string line;
        while (read_line(line)) {
            line = trim(line);
            if (line.empty()) {
                continue;
            }
            if (is_loot_validation_protocol_line(line)) {
                out_line = line;
                return true;
            }
        }
        error = "LootValidationServer closed before sending a protocol response.";
        return false;
    }

    int stdout_read_fd_ = -1;
    int stdin_write_fd_ = -1;
    pid_t child_pid_ = -1;
};
#endif

struct CliArgs {
    uint64_t start = 0;
    bool has_start = false;
    uint64_t count = 2'000'000;
    std::string batch_size = "balanced";
    int max_print = 50;
    std::string mc_version = "26.1.2";
    int strict_workers = 0;
    bool strict_workers_set = false;
    std::string output_mode = "raw";
    uint16_t upper16 = 0;
    bool print_closest = false;
    bool list_structures = false;
    bool dump_query_template = false;
    bool control_stdin = false;
    bool stream_seeds = false;
    bool terminal_mode = false;
    bool debug_enabled = false;
    bool plan_diagnostics = false;
    bool shadow_compare = false;
    std::string completeness_policy;
    std::string compiled_stage_a_mode = "auto";
    bool compiled_stage_a_mode_set = false;
    std::string java_cubiomes_validation = "auto";
    double progress_interval = 1.0;
    std::string execution_mode;
    std::string query_json;
    std::string query_file;
    std::string gpu_pipeline = "auto";  // auto|async|sync
    std::string gpu_backend = "auto";   // auto|cuda|opencl
    std::string gpu_inflight_batches = "auto"; // auto|1..8
    std::string scan_mode = "strict";   // placement|strict|mixed
    uint64_t mixed_queue_capacity = 1'000'000;
    uint32_t mixed_survivor_cap = 0;
    bool mixed_adaptive_throttling = true;
    std::string save_txt;
};

struct JsonValue {
    enum class Type { null_t, bool_t, number_t, string_t, array_t, object_t };
    Type type = Type::null_t;
    bool b = false;
    int64_t number = 0;
    double number_f = 0.0;
    bool number_is_integer = true;
    std::string str;
    std::vector<JsonValue> array;
    std::unordered_map<std::string, JsonValue> object;
};

static std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<char>(c - 'A' + 'a');
        }
        return static_cast<char>(c);
    });
    return s;
}

static std::string trim(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return s.substr(b, e - b);
}

static std::string normalize_key(std::string s) {
    s = lower_ascii(trim(std::move(s)));
    for (char &c : s) {
        if (c == '-' || c == ' ') {
            c = '_';
        }
    }
    return s;
}

static uint32_t fnv1a32_append_bytes(uint32_t hash, const void *data, size_t size) {
    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint32_t>(ptr[i]);
        hash *= 16777619U;
    }
    return hash;
}

static uint32_t fnv1a32_append_u32(uint32_t hash, uint32_t value) {
    return fnv1a32_append_bytes(hash, &value, sizeof(value));
}

static uint32_t fnv1a32_append_i32(uint32_t hash, int32_t value) {
    return fnv1a32_append_bytes(hash, &value, sizeof(value));
}

static uint32_t fnv1a32_append_string(uint32_t hash, const std::string &value) {
    hash = fnv1a32_append_bytes(hash, value.data(), value.size());
    const uint8_t terminator = 0xFFU;
    return fnv1a32_append_bytes(hash, &terminator, sizeof(terminator));
}

static bool parse_custom_strict_gate_multiplier(const std::string &mode_raw, uint32_t &out_multiplier) {
    std::string mode = lower_ascii(trim(mode_raw));
    constexpr const char *prefix = "gate:";
    if (mode.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string value_text = trim(mode.substr(std::strlen(prefix)));
    if (value_text.empty()) {
        throw std::runtime_error("performance.strict_surrogate custom gate requires gate:<positive integer>.");
    }
    try {
        size_t idx = 0;
        const unsigned long long parsed = std::stoull(value_text, &idx, 10);
        if (idx != value_text.size() || parsed == 0ULL ||
            parsed > static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error("");
        }
        out_multiplier = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        throw std::runtime_error("performance.strict_surrogate custom gate requires gate:<positive integer>.");
    }
}

static bool is_supported_strict_surrogate_mode(const std::string &mode_raw) {
    const std::string mode = lower_ascii(trim(mode_raw));
    if (
        mode == "off" || mode == "balanced" || mode == "aggressive" || mode == "ultra" || mode == "turbo" ||
        mode == "lightspeed" || mode == "light-speed"
    ) {
        return true;
    }
    uint32_t gate_multiplier = 0U;
    return parse_custom_strict_gate_multiplier(mode, gate_multiplier);
}

static std::string normalize_execution_mode(std::string mode) {
    mode = lower_ascii(trim(std::move(mode)));
    for (char &c : mode) {
        if (c == '_') {
            c = '-';
        }
    }
    if (mode == "simd4" || mode == "simdx4") {
        return "simd-x4";
    }
    if (mode == "simd8" || mode == "simdx8") {
        return "simd-x8";
    }
    if (mode == "max" || mode == "maxthroughput") {
        return "max-throughput";
    }
    if (
        mode == "auto" || mode == "scalar" || mode == "simd-x4" || mode == "simd-x8" ||
        mode == "max-throughput"
    ) {
        return mode;
    }
    return {};
}

static std::string normalize_sculpt_pattern(std::string pattern) {
    pattern = normalize_key(std::move(pattern));
    if (pattern == "disk" || pattern == "ring" || pattern == "grid" || pattern == "cross") {
        return pattern;
    }
    return {};
}

struct CpuSimdCaps {
    bool avx2 = false;
    bool avx512f = false;
};

static CpuSimdCaps detect_cpu_simd_caps() {
    CpuSimdCaps caps{};
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#if defined(_MSC_VER)
    int cpu_info[4] = {0, 0, 0, 0};
    __cpuid(cpu_info, 0);
    if (cpu_info[0] < 7) {
        return caps;
    }

    __cpuid(cpu_info, 1);
    const bool osxsave = (cpu_info[2] & (1 << 27)) != 0;
    const bool avx = (cpu_info[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) {
        return caps;
    }

    const uint64_t xcr0 = static_cast<uint64_t>(_xgetbv(0));
    if ((xcr0 & 0x6ULL) != 0x6ULL) {
        return caps;
    }

    __cpuidex(cpu_info, 7, 0);
    caps.avx2 = (cpu_info[1] & (1 << 5)) != 0;
    const bool avx512f_hw = (cpu_info[1] & (1 << 16)) != 0;
    caps.avx512f = avx512f_hw && ((xcr0 & 0xE0ULL) == 0xE0ULL);
#elif defined(__GNUC__)
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7U) {
        return caps;
    }
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return caps;
    }
    const bool osxsave = (ecx & bit_OSXSAVE) != 0U;
    const bool avx = (ecx & bit_AVX) != 0U;
    if (!osxsave || !avx) {
        return caps;
    }

    uint32_t xcr0_eax = 0U;
    uint32_t xcr0_edx = 0U;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(xcr0_eax), "=d"(xcr0_edx) : "c"(0));
    const uint64_t xcr0 = (static_cast<uint64_t>(xcr0_edx) << 32U) | static_cast<uint64_t>(xcr0_eax);
    if ((xcr0 & 0x6ULL) != 0x6ULL) {
        return caps;
    }

    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0) {
        return caps;
    }
    caps.avx2 = (ebx & bit_AVX2) != 0U;
    caps.avx512f = ((ebx & bit_AVX512F) != 0U) && ((xcr0 & 0xE0ULL) == 0xE0ULL);
#endif
#endif
    return caps;
}

struct ExecutionModePlan {
    std::string requested = "auto";
    std::string active = "scalar";
    uint32_t simd_width = 1U;
    bool aggressive_unroll = false;
    CpuSimdCaps caps{};
};

static ExecutionModePlan resolve_execution_mode_plan(const std::string &requested_raw) {
    ExecutionModePlan plan{};
    const std::string normalized = normalize_execution_mode(requested_raw);
    plan.requested = normalized.empty() ? "auto" : normalized;
    plan.caps = detect_cpu_simd_caps();

    const std::string best_supported = plan.caps.avx512f ? "simd-x8" : (plan.caps.avx2 ? "simd-x4" : "scalar");
    if (plan.requested == "auto") {
        plan.active = best_supported;
    } else if (plan.requested == "scalar") {
        plan.active = "scalar";
    } else if (plan.requested == "simd-x4") {
        plan.active = plan.caps.avx2 ? "simd-x4" : "scalar";
    } else if (plan.requested == "simd-x8") {
        plan.active = plan.caps.avx512f ? "simd-x8" : (plan.caps.avx2 ? "simd-x4" : "scalar");
    } else if (plan.requested == "max-throughput") {
        plan.active = best_supported;
        plan.aggressive_unroll = true;
    } else {
        plan.active = best_supported;
    }

    if (plan.active == "simd-x8") {
        plan.simd_width = 8U;
    } else if (plan.active == "simd-x4") {
        plan.simd_width = 4U;
    } else {
        plan.simd_width = 1U;
    }
    return plan;
}

enum class CompiledStageAMode {
    auto_mode,
    single,
    multi,
    cpu_a5,
    gpu_off,
};

static std::string normalize_compiled_stage_a_mode(std::string mode) {
    mode = lower_ascii(trim(std::move(mode)));
    for (char &c : mode) {
        if (c == '_') {
            c = '-';
        }
    }
    if (mode.empty()) {
        return "auto";
    }
    if (mode == "auto" || mode == "single" || mode == "multi" || mode == "cpu-a5" || mode == "gpu-off") {
        return mode;
    }
    return {};
}

static CompiledStageAMode parse_compiled_stage_a_mode(const std::string &raw) {
    const std::string normalized = normalize_compiled_stage_a_mode(raw);
    if (normalized == "single") {
        return CompiledStageAMode::single;
    }
    if (normalized == "multi") {
        return CompiledStageAMode::multi;
    }
    if (normalized == "cpu-a5") {
        return CompiledStageAMode::cpu_a5;
    }
    if (normalized == "gpu-off") {
        return CompiledStageAMode::gpu_off;
    }
    return CompiledStageAMode::auto_mode;
}

static const char *compiled_stage_a_mode_name(CompiledStageAMode mode) {
    switch (mode) {
    case CompiledStageAMode::single:
        return "single";
    case CompiledStageAMode::multi:
        return "multi";
    case CompiledStageAMode::cpu_a5:
        return "cpu-a5";
    case CompiledStageAMode::gpu_off:
        return "gpu-off";
    case CompiledStageAMode::auto_mode:
    default:
        return "auto";
    }
}

static bool compiled_stage_a_mode_uses_gpu(CompiledStageAMode mode) {
    return mode == CompiledStageAMode::auto_mode || mode == CompiledStageAMode::single ||
           mode == CompiledStageAMode::multi;
}

static std::string normalize_completeness_policy(std::string policy) {
    policy = lower_ascii(trim(std::move(policy)));
    if (policy.empty()) {
        return "exact";
    }
    if (policy == "safe") {
        return "exact";
    }
    if (policy == "fast" || policy == "lossy" || policy == "approx") {
        return "approximate";
    }
    if (policy == "exact" || policy == "approximate") {
        return policy;
    }
    return {};
}

static bool is_exact_policy(const QuerySpec &spec) {
    return spec.safety.completeness == "exact";
}

static std::string json_escape(std::string value) {
    std::string out;
    out.reserve(value.size() + 8U);
    for (char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    return out;
}

static std::string commaize_digits(std::string digits) {
    if (digits.size() <= 3) {
        return digits;
    }
    const int first_group = static_cast<int>(digits.size() % 3U);
    std::string out;
    out.reserve(digits.size() + (digits.size() / 3U));
    size_t idx = 0;
    if (first_group != 0) {
        out.append(digits, 0, static_cast<size_t>(first_group));
        idx = static_cast<size_t>(first_group);
        if (idx < digits.size()) {
            out.push_back(',');
        }
    }
    while (idx < digits.size()) {
        out.append(digits, idx, 3);
        idx += 3;
        if (idx < digits.size()) {
            out.push_back(',');
        }
    }
    return out;
}

static std::string fmt_u64(uint64_t value) {
    return commaize_digits(std::to_string(value));
}

static std::string fmt_seed(uint64_t value) {
    return std::to_string(value);
}

static std::string fmt_i64(int64_t value) {
    if (value < 0) {
        const uint64_t mag = static_cast<uint64_t>(-(value + 1)) + 1U;
        return "-" + commaize_digits(std::to_string(mag));
    }
    return commaize_digits(std::to_string(static_cast<uint64_t>(value)));
}

static std::string fmt_size(size_t value) {
    return fmt_u64(static_cast<uint64_t>(value));
}

static std::string fmt_double(double value, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(std::max(0, precision)) << value;
    std::string s = oss.str();
    size_t dot = s.find('.');
    std::string int_part = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac_part = (dot == std::string::npos) ? "" : s.substr(dot);

    bool neg = false;
    if (!int_part.empty() && int_part.front() == '-') {
        neg = true;
        int_part.erase(int_part.begin());
    }
    std::string grouped = commaize_digits(int_part);
    if (neg) {
        grouped.insert(grouped.begin(), '-');
    }
    return grouped + frac_part;
}

static std::unordered_map<std::string, StructurePreset> build_structure_presets() {
    return {
        {"village", {34, 8, 10387312, SCAN_SPREAD_LINEAR}},
        {"desert_pyramid", {32, 8, 14357617, SCAN_SPREAD_LINEAR}},
        {"igloo", {32, 8, 14357618, SCAN_SPREAD_LINEAR}},
        {"jungle_temple", {32, 8, 14357619, SCAN_SPREAD_LINEAR}},
        {"swamp_hut", {32, 8, 14357620, SCAN_SPREAD_LINEAR}},
        {"ocean_ruin", {20, 8, 14357621, SCAN_SPREAD_LINEAR}},
        {"shipwreck", {24, 4, 165745295, SCAN_SPREAD_LINEAR}},
        {"ruined_portal", {40, 15, 34222645, SCAN_SPREAD_LINEAR}},
        {"ruined_portal_nether", {40, 15, 34222645, SCAN_SPREAD_LINEAR}},
        {"fortress", {27, 4, 30084232, SCAN_SPREAD_LINEAR}},
        {"bastion_remnant", {27, 4, 30084232, SCAN_SPREAD_LINEAR}},
        {"nether_fossil", {2, 1, 14357921, SCAN_SPREAD_LINEAR}},
        {"ocean_monument", {32, 5, 10387313, SCAN_SPREAD_TRIANGULAR}},
        {"woodland_mansion", {80, 20, 10387319, SCAN_SPREAD_TRIANGULAR}},
        {"end_city", {20, 11, 10387313, SCAN_SPREAD_TRIANGULAR}},
        {"trial_chambers", {34, 12, 94251327, SCAN_SPREAD_LINEAR}},
        {"buried_treasure", {1, 0, 10387320, SCAN_SPREAD_BURIED_TREASURE}},
        {"pillager_outpost", {32, 8, 165745296, SCAN_SPREAD_LINEAR}},
        {"ancient_city", {24, 8, 20083232, SCAN_SPREAD_LINEAR}},
        {"trail_ruin", {34, 8, 83469867, SCAN_SPREAD_LINEAR}},
    };
}

static std::unordered_map<std::string, StructurePreset> build_strict_surrogate_presets() {
    return {};
}

static std::unordered_map<std::string, int> build_structure_ids() {
    return {
        {"desert_pyramid", 1},
        {"jungle_temple", 2},
        {"swamp_hut", 3},
        {"igloo", 4},
        {"village", 5},
        {"ocean_ruin", 6},
        {"shipwreck", 7},
        {"ocean_monument", 8},
        {"woodland_mansion", 9},
        {"pillager_outpost", 10},
        {"ruined_portal", 11},
        {"ruined_portal_nether", 12},
        {"ancient_city", 13},
        {"buried_treasure", 14},
        {"mineshaft", 15},
        {"desert_well", 16},
        {"geode", 17},
        {"fortress", 18},
        {"bastion_remnant", 19},
        {"end_city", 20},
        {"trail_ruin", 23},
        {"trial_chambers", 24},
        {"nether_fossil", 25},
        {"stronghold", -1},
    };
}

static std::unordered_map<std::string, uint32_t> build_strict_spacing_hints() {
    return {
        {"pillager_outpost", 32},
        {"buried_treasure", 1},
        {"ancient_city", 24},
        {"trail_ruin", 34},
        {"mineshaft", 1},
        {"desert_well", 1},
        {"geode", 1},
        {"stronghold", 1},
    };
}

static uint32_t tuned_strict_spacing_hint(
    const std::string &structure,
    uint32_t base_hint,
    const std::string &strict_surrogate_mode
) {
    std::string mode = lower_ascii(trim(strict_surrogate_mode));
    if (mode == "light-speed") {
        mode = "lightspeed";
    }
    if (structure != "mineshaft") {
        return std::max<uint32_t>(1U, base_hint);
    }
    if (mode == "turbo") {
        return std::max<uint32_t>(4U, base_hint);
    }
    if (mode == "lightspeed") {
        return std::max<uint32_t>(8U, base_hint);
    }
    return std::max<uint32_t>(1U, base_hint);
}

static uint32_t quad_max_span_from_accuracy(uint32_t accuracy) {
    const uint32_t a = std::min<uint32_t>(100U, accuracy);
    if (a == 0U) {
        return 0U;
    }
    // Higher accuracy requires tighter clustering for the four hits.
    // 0 -> disabled, 100 -> span ~180 blocks.
    return 512U - ((a * 332U) / 100U);
}

static std::unordered_map<std::string, int> build_strict_surrogate_hash_divisor() {
    return {
        {"village", 2},
        {"desert_pyramid", 2},
        {"igloo", 2},
        {"jungle_temple", 2},
        {"swamp_hut", 2},
        {"ocean_ruin", 2},
        {"shipwreck", 2},
        {"ruined_portal", 2},
        {"ocean_monument", 2},
        {"woodland_mansion", 2},
        {"trial_chambers", 8},
        {"pillager_outpost", 4},
        {"trail_ruin", 8},
        {"ancient_city", 16},
        {"buried_treasure", 32},
        {"mineshaft", 32},
        {"desert_well", 16},
        {"geode", 16},
        {"stronghold", 8},
    };
}

static std::unordered_map<std::string, uint32_t> build_strict_gate_only_salts() {
    return {
        {"buried_treasure", 10387320U},
        {"mineshaft", 0U},
        {"desert_well", 40002U},
        {"geode", 20002U},
        {"stronghold", 30001U},
    };
}

static std::unordered_map<std::string, std::string> build_structure_aliases() {
    return {
        {"village", "village"},
        {"villages", "village"},
        {"village_plains", "village_plains"},
        {"village_desert", "village_desert"},
        {"village_savanna", "village_savanna"},
        {"village_snowy", "village_snowy"},
        {"village_taiga", "village_taiga"},
        {"plains_village", "village_plains"},
        {"desert_village", "village_desert"},
        {"savanna_village", "village_savanna"},
        {"snowy_village", "village_snowy"},
        {"taiga_village", "village_taiga"},
        {"desert_pyramid", "desert_pyramid"},
        {"desert_temple", "desert_pyramid"},
        {"desert", "desert_pyramid"},
        {"igloo", "igloo"},
        {"jungle_temple", "jungle_temple"},
        {"jungle_pyramid", "jungle_temple"},
        {"swamp_hut", "swamp_hut"},
        {"witch_hut", "swamp_hut"},
        {"quad_hut", "quad_swamp_hut"},
        {"quadhut", "quad_swamp_hut"},
        {"quad_swamp_hut", "quad_swamp_hut"},
        {"quad_witch_hut", "quad_swamp_hut"},
        {"ocean_ruin", "ocean_ruin"},
        {"ocean_ruin_cold", "ocean_ruin"},
        {"ocean_ruin_warm", "ocean_ruin"},
        {"underwater_ruin", "ocean_ruin"},
        {"shipwreck", "shipwreck"},
        {"shipwreck_beached", "shipwreck"},
        {"beached_shipwreck", "shipwreck"},
        {"ruined_portal", "ruined_portal"},
        {"ruined_portal_desert", "ruined_portal_desert"},
        {"ruined_portal_jungle", "ruined_portal_jungle"},
        {"ruined_portal_mountain", "ruined_portal_mountain"},
        {"ruined_portal_ocean", "ruined_portal_ocean"},
        {"ruined_portal_swamp", "ruined_portal_swamp"},
        {"ruined_portal_nether", "ruined_portal_nether"},
        {"ruined_portal_n", "ruined_portal_nether"},
        {"nether_ruined_portal", "ruined_portal_nether"},
        {"fortress", "fortress"},
        {"nether_fortress", "fortress"},
        {"bastion", "bastion_remnant"},
        {"bastion_remnant", "bastion_remnant"},
        {"nether_fossil", "nether_fossil"},
        {"fossil_nether", "nether_fossil"},
        {"ocean_monument", "ocean_monument"},
        {"monument", "ocean_monument"},
        {"quad_monument", "quad_ocean_monument"},
        {"quad_ocean_monument", "quad_ocean_monument"},
        {"woodland_mansion", "woodland_mansion"},
        {"mansion", "woodland_mansion"},
        {"end_city", "end_city"},
        {"end_cities", "end_city"},
        {"quad_desert_pyramid", "quad_desert_pyramid"},
        {"quad_desert_temple", "quad_desert_pyramid"},
        {"quad_igloo", "quad_igloo"},
        {"quad_jungle_temple", "quad_jungle_temple"},
        {"quad_jungle_pyramid", "quad_jungle_temple"},
        {"trial_chambers", "trial_chambers"},
        {"trial_chamber", "trial_chambers"},
        {"pillager_outpost", "pillager_outpost"},
        {"outpost", "pillager_outpost"},
        {"buried_treasure", "buried_treasure"},
        {"treasure", "buried_treasure"},
        {"ancient_city", "ancient_city"},
        {"trail_ruin", "trail_ruin"},
        {"trail_ruins", "trail_ruin"},
        {"mineshaft", "mineshaft"},
        {"abandoned_mineshaft", "mineshaft"},
        {"mineshaft_mesa", "mineshaft"},
        {"mesa_mineshaft", "mineshaft"},
        {"badlands_mineshaft", "mineshaft"},
        {"desert_well", "desert_well"},
        {"geode", "geode"},
        {"stronghold", "stronghold"},
    };
}

static const std::unordered_map<std::string, StructurePreset> kStructurePresets = build_structure_presets();
static const std::unordered_map<std::string, StructurePreset> kStrictSurrogatePresets =
    build_strict_surrogate_presets();
static const std::unordered_map<std::string, int> kStructureIds = build_structure_ids();
static const std::unordered_map<std::string, uint32_t> kStrictSpacingHints = build_strict_spacing_hints();
static const std::unordered_map<std::string, int> kStrictSurrogateHashDivisor =
    build_strict_surrogate_hash_divisor();
static const std::unordered_map<std::string, uint32_t> kStrictGateOnlySalts =
    build_strict_gate_only_salts();
static const std::unordered_map<std::string, std::string> kStructureAliases = build_structure_aliases();

static bool supports_direct_placement_mode(const std::string &structure) {
    const auto it = kStructureIds.find(structure);
    return it != kStructureIds.end() && it->second >= 0;
}

static bool supports_exact_attempt_prefilter(const std::string &structure) {
    // Strict and mixed scans may use exact random-spread structure placement
    // as a lossless Stage A prefilter. Final CPU/Java validation still decides
    // biome, terrain, loot, and variant-specific correctness.
    return kStructurePresets.find(structure) != kStructurePresets.end() ||
           kStrictSurrogatePresets.find(structure) != kStrictSurrogatePresets.end();
}

static bool supports_placement_mode(const std::string &structure) {
    if (structure == "stronghold") {
        // Strongholds use the native cubiomes ring iterator even in placement mode.
        return true;
    }
    return kStructurePresets.find(structure) != kStructurePresets.end() || supports_direct_placement_mode(structure);
}

class JsonParser {
public:
    explicit JsonParser(const std::string &text) : text_(text) {}

    JsonValue parse() {
        JsonValue out = parse_value();
        skip_ws();
        if (pos_ != text_.size()) {
            throw std::runtime_error("Unexpected trailing JSON text.");
        }
        return out;
    }

private:
    const std::string &text_;
    size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char peek() const {
        if (pos_ >= text_.size()) {
            return '\0';
        }
        return text_[pos_];
    }

    char take() {
        if (pos_ >= text_.size()) {
            throw std::runtime_error("Unexpected end of JSON.");
        }
        return text_[pos_++];
    }

    void expect(char c) {
        char got = take();
        if (got != c) {
            std::ostringstream oss;
            oss << "Expected '" << c << "' in JSON.";
            throw std::runtime_error(oss.str());
        }
    }

    JsonValue parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') {
            return parse_object();
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == '"') {
            JsonValue v;
            v.type = JsonValue::Type::string_t;
            v.str = parse_string();
            return v;
        }
        if (c == 't' || c == 'f') {
            return parse_bool();
        }
        if (c == 'n') {
            return parse_null();
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number();
        }
        throw std::runtime_error("Invalid JSON value.");
    }

    JsonValue parse_object() {
        JsonValue v;
        v.type = JsonValue::Type::object_t;
        expect('{');
        skip_ws();
        if (peek() == '}') {
            take();
            return v;
        }
        while (true) {
            skip_ws();
            if (peek() != '"') {
                throw std::runtime_error("Object key must be a JSON string.");
            }
            std::string key = parse_string();
            skip_ws();
            expect(':');
            JsonValue value = parse_value();
            v.object.emplace(std::move(key), std::move(value));
            skip_ws();
            char c = take();
            if (c == '}') {
                break;
            }
            if (c != ',') {
                throw std::runtime_error("Expected ',' or '}' in JSON object.");
            }
        }
        return v;
    }

    JsonValue parse_array() {
        JsonValue v;
        v.type = JsonValue::Type::array_t;
        expect('[');
        skip_ws();
        if (peek() == ']') {
            take();
            return v;
        }
        while (true) {
            v.array.push_back(parse_value());
            skip_ws();
            char c = take();
            if (c == ']') {
                break;
            }
            if (c != ',') {
                throw std::runtime_error("Expected ',' or ']' in JSON array.");
            }
        }
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            char c = take();
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                char esc = take();
                switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    uint32_t code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = take();
                        code <<= 4U;
                        if (h >= '0' && h <= '9') {
                            code |= static_cast<uint32_t>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            code |= static_cast<uint32_t>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            code |= static_cast<uint32_t>(h - 'A' + 10);
                        } else {
                            throw std::runtime_error("Invalid \\u escape in JSON string.");
                        }
                    }
                    if (code <= 0x7F) {
                        out.push_back(static_cast<char>(code));
                    } else {
                        out.push_back('?');
                    }
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported escape in JSON string.");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    JsonValue parse_bool() {
        JsonValue v;
        v.type = JsonValue::Type::bool_t;
        if (text_.compare(pos_, 4, "true") == 0) {
            v.b = true;
            pos_ += 4;
            return v;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            v.b = false;
            pos_ += 5;
            return v;
        }
        throw std::runtime_error("Invalid boolean JSON literal.");
    }

    JsonValue parse_null() {
        if (text_.compare(pos_, 4, "null") != 0) {
            throw std::runtime_error("Invalid null JSON literal.");
        }
        pos_ += 4;
        JsonValue v;
        v.type = JsonValue::Type::null_t;
        return v;
    }

    JsonValue parse_number() {
        size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (!(peek() >= '0' && peek() <= '9')) {
            throw std::runtime_error("Invalid JSON number.");
        }
        while (peek() >= '0' && peek() <= '9') {
            ++pos_;
        }
        bool is_float = false;
        if (peek() == '.') {
            is_float = true;
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) {
                throw std::runtime_error("Invalid JSON number.");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            if (!(peek() >= '0' && peek() <= '9')) {
                throw std::runtime_error("Invalid JSON number.");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        const std::string raw = text_.substr(start, pos_ - start);
        JsonValue v;
        v.type = JsonValue::Type::number_t;
        try {
            if (is_float) {
                v.number_f = std::stod(raw);
                v.number = static_cast<int64_t>(v.number_f);
                v.number_is_integer = false;
            } else {
                v.number = std::stoll(raw);
                v.number_f = static_cast<double>(v.number);
                v.number_is_integer = true;
            }
        } catch (...) {
            throw std::runtime_error("Invalid JSON number.");
        }
        return v;
    }
};

static const JsonValue *json_get(const JsonValue &obj, const std::string &key) {
    if (obj.type != JsonValue::Type::object_t) {
        return nullptr;
    }
    const auto it = obj.object.find(key);
    if (it == obj.object.end()) {
        return nullptr;
    }
    return &it->second;
}

static std::string json_to_string(const JsonValue *v, const std::string &fallback = "") {
    if (v == nullptr) {
        return fallback;
    }
    if (v->type == JsonValue::Type::string_t) {
        return v->str;
    }
    if (v->type == JsonValue::Type::number_t) {
        if (v->number_is_integer) {
            return std::to_string(v->number);
        }
        std::ostringstream oss;
        oss << std::setprecision(12) << v->number_f;
        return oss.str();
    }
    return fallback;
}

static int json_to_int(const JsonValue *v, int fallback = 0) {
    if (v == nullptr) {
        return fallback;
    }
    if (v->type == JsonValue::Type::number_t) {
        return v->number_is_integer ? static_cast<int>(v->number) : static_cast<int>(v->number_f);
    }
    if (v->type == JsonValue::Type::string_t) {
        try {
            return std::stoi(v->str, nullptr, 0);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

static double json_to_double(const JsonValue *v, double fallback = 0.0) {
    if (v == nullptr) {
        return fallback;
    }
    if (v->type == JsonValue::Type::number_t) {
        return v->number_is_integer ? static_cast<double>(v->number) : v->number_f;
    }
    if (v->type == JsonValue::Type::string_t) {
        try {
            size_t idx = 0;
            const std::string raw = trim(v->str);
            const double d = std::stod(raw, &idx);
            if (idx == raw.size()) {
                return d;
            }
        } catch (...) {
        }
    }
    return fallback;
}

static bool json_is_object(const JsonValue *v) {
    return v != nullptr && v->type == JsonValue::Type::object_t;
}

static bool json_is_array(const JsonValue *v) {
    return v != nullptr && v->type == JsonValue::Type::array_t;
}

static bool json_to_bool(const JsonValue *v, bool fallback = false) {
    if (v == nullptr) {
        return fallback;
    }
    if (v->type == JsonValue::Type::bool_t) {
        return v->b;
    }
    if (v->type == JsonValue::Type::number_t) {
        return v->number_is_integer ? (v->number != 0) : (v->number_f != 0.0);
    }
    if (v->type == JsonValue::Type::string_t) {
        const std::string s = lower_ascii(trim(v->str));
        if (s == "1" || s == "true" || s == "yes" || s == "on") {
            return true;
        }
        if (s == "0" || s == "false" || s == "no" || s == "off") {
            return false;
        }
    }
    return fallback;
}

static int32_t json_to_i32_checked(const JsonValue *v, const std::string &field_name) {
    if (v == nullptr) {
        throw std::runtime_error(field_name + " is required.");
    }
    long long n = 0;
    if (v->type == JsonValue::Type::number_t) {
        if (!v->number_is_integer) {
            throw std::runtime_error(field_name + " must be an integer.");
        }
        n = v->number;
    } else if (v->type == JsonValue::Type::string_t) {
        try {
            const std::string raw = trim(v->str);
            size_t idx = 0;
            n = std::stoll(raw, &idx, 0);
            if (idx != raw.size()) {
                throw std::runtime_error("");
            }
        } catch (...) {
            throw std::runtime_error(field_name + " must be an integer.");
        }
    } else {
        throw std::runtime_error(field_name + " must be an integer.");
    }
    if (
        n < static_cast<long long>(std::numeric_limits<int32_t>::min()) ||
        n > static_cast<long long>(std::numeric_limits<int32_t>::max())
    ) {
        throw std::runtime_error(field_name + " is out of int32 range.");
    }
    return static_cast<int32_t>(n);
}

static std::vector<LootRequirementSpec> parse_loot_requirements_json(
    const JsonValue *required,
    const std::string &field_name
) {
    if (required == nullptr) {
        throw std::runtime_error(field_name + " is required.");
    }

    std::unordered_map<std::string, int> merged;
    auto add_requirement = [&](std::string item_raw, int count) {
        const std::string item = normalize_loot_item_id(std::move(item_raw));
        if (item.empty()) {
            throw std::runtime_error(field_name + " contains an empty item id.");
        }
        if (count <= 0) {
            throw std::runtime_error(field_name + " counts must be > 0.");
        }
        merged[item] += count;
    };

    if (required->type == JsonValue::Type::object_t) {
        for (const auto &kv : required->object) {
            add_requirement(kv.first, json_to_int(&kv.second, 0));
        }
    } else if (required->type == JsonValue::Type::array_t) {
        for (const JsonValue &entry : required->array) {
            if (entry.type == JsonValue::Type::string_t) {
                const std::string raw = trim(entry.str);
                if (raw.empty()) {
                    continue;
                }
                const size_t colon = raw.rfind(':');
                if (colon == std::string::npos) {
                    add_requirement(raw, 1);
                } else {
                    int count = 0;
                    try {
                        count = std::stoi(trim(raw.substr(colon + 1U)), nullptr, 0);
                    } catch (const std::exception &) {
                        throw std::runtime_error(field_name + " contains an invalid count entry: " + raw);
                    }
                    add_requirement(raw.substr(0, colon), count);
                }
                continue;
            }
            if (entry.type == JsonValue::Type::object_t) {
                const std::string item = json_to_string(json_get(entry, "item"), "");
                const int count = json_to_int(json_get(entry, "count"), 0);
                add_requirement(item, count);
                continue;
            }
            throw std::runtime_error(
                field_name + " array entries must be strings like diamond:2 or objects {item,count}."
            );
        }
    } else {
        throw std::runtime_error(field_name + " must be an object or array.");
    }

    if (merged.empty()) {
        throw std::runtime_error(field_name + " must not be empty.");
    }

    std::vector<LootRequirementSpec> out;
    out.reserve(merged.size());
    for (const auto &kv : merged) {
        LootRequirementSpec spec{};
        spec.item = kv.first;
        spec.count = kv.second;
        out.push_back(std::move(spec));
    }
    std::sort(out.begin(), out.end(), [](const LootRequirementSpec &a, const LootRequirementSpec &b) {
        return a.item < b.item;
    });
    return out;
}

static std::vector<std::string> parse_structure_setting_values(
    const JsonValue *raw,
    const std::string &field_name
) {
    std::vector<std::string> values;
    if (raw == nullptr || raw->type == JsonValue::Type::null_t) {
        return values;
    }
    auto add_value = [&](std::string value) {
        value = normalize_loot_structure_id(std::move(value));
        if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(std::move(value));
        }
    };
    if (raw->type == JsonValue::Type::string_t) {
        add_value(raw->str);
    } else if (raw->type == JsonValue::Type::array_t) {
        for (const JsonValue &entry : raw->array) {
            if (entry.type != JsonValue::Type::string_t) {
                throw std::runtime_error(field_name + " entries must be strings.");
            }
            add_value(entry.str);
        }
    } else {
        throw std::runtime_error(field_name + " must be a string or string array.");
    }
    return values;
}

static std::unordered_map<std::string, std::vector<std::string>> parse_structure_settings_json(
    const JsonValue *settings,
    const std::string &field_name
) {
    std::unordered_map<std::string, std::vector<std::string>> out;
    if (settings == nullptr || settings->type == JsonValue::Type::null_t) {
        return out;
    }
    if (!json_is_object(settings)) {
        throw std::runtime_error(field_name + " must be an object.");
    }
    for (const auto &kv : settings->object) {
        const std::string group = normalize_loot_structure_id(kv.first);
        if (group.empty()) {
            continue;
        }
        std::vector<std::string> values = parse_structure_setting_values(&kv.second, field_name + "." + group);
        if (!values.empty()) {
            out[group] = std::move(values);
        }
    }
    return out;
}

static std::string encode_structure_settings_for_server(
    const std::unordered_map<std::string, std::vector<std::string>> &settings
) {
    if (settings.empty()) {
        return "_";
    }
    std::vector<std::string> groups;
    groups.reserve(settings.size());
    for (const auto &kv : settings) {
        if (!kv.second.empty()) {
            groups.push_back(kv.first);
        }
    }
    std::sort(groups.begin(), groups.end());
    std::ostringstream oss;
    bool first_group = true;
    for (const std::string &group : groups) {
        const auto it = settings.find(group);
        if (it == settings.end() || it->second.empty()) {
            continue;
        }
        std::vector<std::string> values = it->second;
        std::sort(values.begin(), values.end());
        if (!first_group) {
            oss << ';';
        }
        first_group = false;
        oss << group << '=';
        for (size_t i = 0; i < values.size(); ++i) {
            if (i != 0U) {
                oss << ',';
            }
            oss << values[i];
        }
    }
    const std::string encoded = oss.str();
    return encoded.empty() ? std::string("_") : encoded;
}

static std::pair<int32_t, int32_t> parse_shape_center_xz(const JsonValue *center, const std::string &field_name) {
    if (center == nullptr) {
        throw std::runtime_error(field_name + " is required.");
    }
    if (center->type == JsonValue::Type::array_t) {
        if (center->array.size() < 2U) {
            throw std::runtime_error(field_name + " array must have [x, z].");
        }
        const int32_t x = json_to_i32_checked(&center->array[0], field_name + "[0]");
        const int32_t z = json_to_i32_checked(&center->array[1], field_name + "[1]");
        return {x, z};
    }
    if (center->type == JsonValue::Type::object_t) {
        const int32_t x = json_to_i32_checked(json_get(*center, "x"), field_name + ".x");
        const int32_t z = json_to_i32_checked(json_get(*center, "z"), field_name + ".z");
        return {x, z};
    }
    throw std::runtime_error(field_name + " must be an array [x,z] or object {x,z}.");
}

static bool parse_fixed_anchor_coords(const std::string &anchor, int32_t &out_x, int32_t &out_z) {
    const std::string raw = trim(anchor);
    const std::string low = lower_ascii(raw);
    constexpr const char *prefix = "fixed:";
    if (low.rfind(prefix, 0) != 0) {
        return false;
    }

    const std::string payload = trim(raw.substr(std::strlen(prefix)));
    const size_t comma = payload.find(',');
    if (comma == std::string::npos) {
        throw std::runtime_error("Invalid fixed anchor '" + anchor + "'. Expected fixed:x,z.");
    }
    const std::string xs = trim(payload.substr(0, comma));
    const std::string zs = trim(payload.substr(comma + 1));
    if (xs.empty() || zs.empty()) {
        throw std::runtime_error("Invalid fixed anchor '" + anchor + "'. Expected fixed:x,z.");
    }

    try {
        size_t x_idx = 0;
        size_t z_idx = 0;
        const long long x_ll = std::stoll(xs, &x_idx, 0);
        const long long z_ll = std::stoll(zs, &z_idx, 0);
        if (x_idx != xs.size() || z_idx != zs.size()) {
            throw std::runtime_error("");
        }
        if (
            x_ll < static_cast<long long>(std::numeric_limits<int32_t>::min()) ||
            x_ll > static_cast<long long>(std::numeric_limits<int32_t>::max()) ||
            z_ll < static_cast<long long>(std::numeric_limits<int32_t>::min()) ||
            z_ll > static_cast<long long>(std::numeric_limits<int32_t>::max())
        ) {
            throw std::runtime_error("");
        }
        out_x = static_cast<int32_t>(x_ll);
        out_z = static_cast<int32_t>(z_ll);
        return true;
    } catch (...) {
        throw std::runtime_error("Invalid fixed anchor '" + anchor + "'. Expected fixed:x,z.");
    }
}

static std::string parse_anchor_ref(const std::string &anchor) {
    const std::string raw = trim(anchor);
    const std::string low = lower_ascii(raw);
    if (low == "origin" || low == "spawn") {
        return low;
    }
    int32_t fixed_x = 0;
    int32_t fixed_z = 0;
    if (parse_fixed_anchor_coords(raw, fixed_x, fixed_z)) {
        return "fixed:" + std::to_string(fixed_x) + "," + std::to_string(fixed_z);
    }
    constexpr const char *prefix = "constraint:";
    if (low.rfind(prefix, 0) == 0 && raw.size() > std::strlen(prefix)) {
        std::string dep = trim(raw.substr(std::strlen(prefix)));
        if (!dep.empty()) {
            return "constraint:" + dep;
        }
    }
    throw std::runtime_error("Invalid anchor reference '" + anchor + "'. Use origin, spawn, fixed:x,z, or constraint:<id>.");
}

static std::string normalize_dimension_name(const std::string &dimension) {
    const std::string key = normalize_key(dimension);
    if (key.empty() || key == "overworld" || key == "world" || key == "surface") {
        return "overworld";
    }
    if (key == "nether" || key == "the_nether" || key == "hell") {
        return "nether";
    }
    if (key == "end" || key == "the_end") {
        return "end";
    }
    throw std::runtime_error("Invalid dimension '" + dimension + "'. Use overworld, nether, or end.");
}

static int32_t dimension_id_from_name(const std::string &dimension) {
    const std::string normalized = normalize_dimension_name(dimension);
    if (normalized == "nether") {
        return SCAN_DIM_NETHER;
    }
    if (normalized == "end") {
        return SCAN_DIM_END;
    }
    return SCAN_DIM_OVERWORLD;
}

static std::string normalize_structure_name(const std::string &name) {
    const std::string key = normalize_key(name);
    const auto it = kStructureAliases.find(key);
    if (it == kStructureAliases.end()) {
        throw std::runtime_error("Unknown structure '" + name + "'.");
    }
    return it->second;
}

struct StructureVariantResolution {
    std::string structure;
    uint32_t min_required = 1U;
    uint32_t viability_flags = 0U;
    bool strict_only_variant = false;
    std::string validation_structure;
};

static uint32_t biome_flag_or_throw(
    const std::unordered_map<std::string, int32_t> &biome_map,
    const std::string &biome_key
) {
    const auto it = biome_map.find(biome_key);
    if (it == biome_map.end() || it->second < 0) {
        throw std::runtime_error("Biome constant '" + biome_key + "' is unavailable in this cubiomes build.");
    }
    return static_cast<uint32_t>(it->second);
}

static StructureVariantResolution resolve_structure_variant(
    const std::string &name,
    const std::unordered_map<std::string, int32_t> &biome_map
) {
    const std::string normalized = normalize_structure_name(name);
    if (normalized == "village_plains") {
        return {"village", 1U, biome_flag_or_throw(biome_map, "plains"), true};
    }
    if (normalized == "village_desert") {
        return {"village", 1U, biome_flag_or_throw(biome_map, "desert"), true};
    }
    if (normalized == "village_savanna") {
        return {"village", 1U, biome_flag_or_throw(biome_map, "savanna"), true};
    }
    if (normalized == "village_snowy") {
        const auto snowy_it = biome_map.find("snowy_tundra");
        if (snowy_it != biome_map.end() && snowy_it->second >= 0) {
            return {"village", 1U, static_cast<uint32_t>(snowy_it->second), true};
        }
        return {"village", 1U, biome_flag_or_throw(biome_map, "snowy_plains"), true};
    }
    if (normalized == "village_taiga") {
        return {"village", 1U, biome_flag_or_throw(biome_map, "taiga"), true};
    }
    if (normalized == "ruined_portal_desert" || normalized == "ruined_portal_jungle" ||
        normalized == "ruined_portal_mountain" || normalized == "ruined_portal_ocean" ||
        normalized == "ruined_portal_swamp") {
        return {"ruined_portal", 1U, 0U, true, normalized};
    }
    if (normalized == "quad_swamp_hut") {
        return {"swamp_hut", 4U, 0U, true};
    }
    if (normalized == "quad_ocean_monument") {
        return {"ocean_monument", 4U, 0U, true};
    }
    if (normalized == "quad_desert_pyramid") {
        return {"desert_pyramid", 4U, 0U, true};
    }
    if (normalized == "quad_igloo") {
        return {"igloo", 4U, 0U, true};
    }
    if (normalized == "quad_jungle_temple") {
        return {"jungle_temple", 4U, 0U, true};
    }
    return {normalized, 1U, 0U, false};
}

static std::string default_dimension_for_structure(const std::string &structure) {
    if (
        structure == "fortress" || structure == "bastion_remnant" ||
        structure == "ruined_portal_nether" || structure == "nether_fossil"
    ) {
        return "nether";
    }
    if (structure == "end_city") {
        return "end";
    }
    return "overworld";
}

static bool structure_dimension_compatible(const std::string &structure, const std::string &dimension) {
    const std::string dim = normalize_dimension_name(dimension);
    if (structure == "fortress" || structure == "bastion_remnant" ||
        structure == "ruined_portal_nether" || structure == "nether_fossil") {
        return dim == "nether";
    }
    if (structure == "end_city") {
        return dim == "end";
    }
    if (structure == "stronghold") {
        return dim == "overworld";
    }
    return dim == "overworld";
}

static int resolve_mc_version(const std::string &version_text) {
    const std::string v = lower_ascii(trim(version_text));
    if (v == "1.20" || v == "1.20.x" || v == "1.20.6") {
        return 25;
    }
    if (v == "1.21.1") {
        return 26;
    }
    if (v == "1.21.2" || v == "1.21.3") {
        return 27;
    }
    if (v == "1.21" || v == "1.21 wd" || v == "1.21.4" || v == "1.21.5") {
        return 28;
    }
    if (v == "26.1.2" || v == "26.1.1" || v == "1.21.11" || v == "4790") {
        return 29;
    }
    throw std::runtime_error("Unsupported --mc-version value: " + version_text);
}

static int effective_biome_sample_step(const std::string &strict_surrogate, int radius) {
    std::string mode = lower_ascii(trim(strict_surrogate.empty() ? std::string("off") : strict_surrogate));
    if (mode == "light-speed") {
        mode = "lightspeed";
    }
    uint32_t custom_gate_multiplier = 0U;
    if (parse_custom_strict_gate_multiplier(mode, custom_gate_multiplier)) {
        return BIOME_SAMPLE_STEP_BLOCKS;
    }
    const int r = std::max(0, radius);
    if (mode == "off") {
        return BIOME_SAMPLE_STEP_BLOCKS;
    }
    if (mode == "balanced") {
        if (r >= 256) {
            return 10;
        }
        if (r >= 128) {
            return 8;
        }
        if (r >= 64) {
            return 6;
        }
        return BIOME_SAMPLE_STEP_BLOCKS;
    }
    if (mode == "aggressive") {
        if (r >= 256) {
            return 20;
        }
        if (r >= 192) {
            return 16;
        }
        if (r >= 128) {
            return 12;
        }
        if (r >= 64) {
            return 10;
        }
        if (r >= 32) {
            return 8;
        }
        return BIOME_SAMPLE_STEP_BLOCKS;
    }
    if (mode == "ultra") {
        if (r >= 256) {
            return 32;
        }
        if (r >= 192) {
            return 24;
        }
        if (r >= 128) {
            return 20;
        }
        if (r >= 64) {
            return 16;
        }
        if (r >= 32) {
            return 12;
        }
        return BIOME_SAMPLE_STEP_BLOCKS;
    }
    // turbo: extremely lossy biome scan for max throughput.
    if (r >= 256) {
        return 96;
    }
    if (r >= 192) {
        return 80;
    }
    if (r >= 128) {
        return 64;
    }
    if (r >= 64) {
        return 48;
    }
    if (r >= 32) {
        return 32;
    }
    if (mode == "turbo") {
        return 24;
    }
    // lightspeed: even more aggressive.
    if (r >= 256) {
        return 160;
    }
    if (r >= 192) {
        return 128;
    }
    if (r >= 128) {
        return 96;
    }
    if (r >= 64) {
        return 80;
    }
    if (r >= 32) {
        return 64;
    }
    return 48;
}

static uint32_t strict_gate_divisor(const std::string &structure, const std::string &mode) {
    std::string low_mode = lower_ascii(trim(mode));
    if (low_mode == "light-speed") {
        low_mode = "lightspeed";
    }
    uint32_t custom_gate_multiplier = 0U;
    if (parse_custom_strict_gate_multiplier(low_mode, custom_gate_multiplier)) {
        const auto it = kStrictSurrogateHashDivisor.find(structure);
        const int base = it == kStrictSurrogateHashDivisor.end() ? 1 : std::max(1, it->second);
        const uint64_t scaled = static_cast<uint64_t>(std::max(1, base)) * custom_gate_multiplier;
        return static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()),
            std::max<uint64_t>(1ULL, scaled)
        ));
    }
    if (low_mode == "off") {
        return 1U;
    }
    const auto it = kStrictSurrogateHashDivisor.find(structure);
    const int base = it == kStrictSurrogateHashDivisor.end() ? 1 : std::max(1, it->second);
    if (base <= 1) {
        return 1U;
    }

    // Stronghold strict validation is unusually expensive. To keep "turbo"/"lightspeed" usable,
    // apply a much more aggressive gate in those modes.
    if (structure == "stronghold") {
        if (low_mode == "balanced") {
            return static_cast<uint32_t>(base);
        }
        if (low_mode == "aggressive") {
            return static_cast<uint32_t>(base * 2);
        }
        if (low_mode == "ultra") {
            return static_cast<uint32_t>(base * 4);
        }
        if (low_mode == "turbo") {
            return static_cast<uint32_t>(base * 16384);
        }
        return static_cast<uint32_t>(base * 131072);
    }

    if (low_mode == "balanced") {
        return static_cast<uint32_t>(base);
    }
    if (low_mode == "aggressive") {
        return static_cast<uint32_t>(base * 2);
    }
    if (low_mode == "ultra") {
        return static_cast<uint32_t>(base * 4);
    }
    if (low_mode == "turbo") {
        return static_cast<uint32_t>(base * 16);
    }
    return static_cast<uint32_t>(base * 64);
}

static uint32_t biome_gate_multiplier(const QuerySpec &spec) {
    if (spec.biome_filters.empty()) {
        return 1U;
    }
    std::string mode = lower_ascii(trim(spec.perf.strict_surrogate.empty() ? std::string("off") : spec.perf.strict_surrogate));
    if (mode == "light-speed") {
        mode = "lightspeed";
    }
    uint32_t custom_gate_multiplier = 0U;
    if (parse_custom_strict_gate_multiplier(mode, custom_gate_multiplier)) {
        return std::max<uint32_t>(1U, custom_gate_multiplier);
    }
    if (mode != "aggressive" && mode != "ultra" && mode != "turbo" && mode != "lightspeed") {
        return 1U;
    }

    int score = 0;
    for (const BiomeFilterSpec &bf : spec.biome_filters) {
        const int step = effective_biome_sample_step(spec.perf.strict_surrogate, bf.radius);
        const int r = std::max(0, bf.radius);
        const double points =
            (3.14159265358979323846 * static_cast<double>(r) * static_cast<double>(r)) /
            static_cast<double>(std::max(1, step * step));
        score += std::max(1, static_cast<int>(points));
    }

    if (mode == "aggressive") {
        if (score >= 4000) {
            return 16U;
        }
        if (score >= 700) {
            return 8U;
        }
        if (score >= 250) {
            return 4U;
        }
        if (score >= 80) {
            return 2U;
        }
        return 1U;
    }

    if (mode == "ultra") {
        if (score >= 2500) {
            return 64U;
        }
        if (score >= 900) {
            return 32U;
        }
        if (score >= 250) {
            return 16U;
        }
        if (score >= 80) {
            return 8U;
        }
        return 4U;
    }
    if (mode == "turbo") {
        if (score >= 2500) {
            return 512U;
        }
        if (score >= 900) {
            return 256U;
        }
        if (score >= 250) {
            return 128U;
        }
        if (score >= 80) {
            return 64U;
        }
        return 32U;
    }
    if (score >= 2500) {
        return 4096U;
    }
    if (score >= 900) {
        return 2048U;
    }
    if (score >= 250) {
        return 1024U;
    }
    if (score >= 80) {
        return 512U;
    }
    return 256U;
}

static uint32_t biome_gate_salt(const QuerySpec &spec) {
    uint32_t hash = 2166136261U;
    static constexpr char kPrefix[] = "biome-gate";
    hash = fnv1a32_append_bytes(hash, kPrefix, sizeof(kPrefix) - 1U);
    for (const BiomeFilterSpec &bf : spec.biome_filters) {
        hash = fnv1a32_append_string(hash, bf.point);
        hash = fnv1a32_append_i32(hash, static_cast<int32_t>(bf.y));
        hash = fnv1a32_append_i32(hash, static_cast<int32_t>(bf.radius));
        hash = fnv1a32_append_u32(hash, static_cast<uint32_t>(bf.allowed_ids.size()));
        for (int32_t id : bf.allowed_ids) {
            hash = fnv1a32_append_i32(hash, id);
        }
    }
    return hash;
}

static int estimated_loot_filter_score(const QuerySpec &spec) {
    if (spec.loot_filters.empty()) {
        return 0;
    }

    int score = 0;
    for (const LootFilterSpec &lf : spec.loot_filters) {
        const std::string structure = normalize_loot_structure_id(lf.structure);
        int structure_cost = 6;
        if (structure == "buried_treasure" || structure == "auto") {
            structure_cost = 4;
        } else if (structure == "desert_pyramid") {
            structure_cost = 8;
        } else if (
            structure == "shipwreck" || structure == "shipwreck_supply" || structure == "shipwreck_map" ||
            structure == "shipwreck_treasure"
        ) {
            structure_cost = 12;
        } else if (structure == "ruined_portal") {
            structure_cost = 10;
        }

        int requirement_cost = 0;
        for (const LootRequirementSpec &req : lf.required) {
            requirement_cost += std::max(1, req.count);
        }
        requirement_cost = std::max<int>(1, requirement_cost);
        score += structure_cost * requirement_cost;
    }
    return score;
}

static uint32_t loot_gate_multiplier(const QuerySpec &spec) {
    if (spec.loot_filters.empty()) {
        return 1U;
    }
    std::string mode = lower_ascii(trim(spec.perf.strict_surrogate.empty() ? std::string("off") : spec.perf.strict_surrogate));
    if (mode == "light-speed") {
        mode = "lightspeed";
    }
    uint32_t custom_gate_multiplier = 0U;
    if (parse_custom_strict_gate_multiplier(mode, custom_gate_multiplier)) {
        return std::max<uint32_t>(1U, custom_gate_multiplier);
    }
    if (mode != "aggressive" && mode != "ultra" && mode != "turbo" && mode != "lightspeed") {
        return 1U;
    }

    const int score = estimated_loot_filter_score(spec);
    if (mode == "aggressive") {
        if (score >= 48) {
            return 8U;
        }
        if (score >= 24) {
            return 4U;
        }
        if (score >= 12) {
            return 2U;
        }
        return 1U;
    }
    if (mode == "ultra") {
        if (score >= 64) {
            return 64U;
        }
        if (score >= 32) {
            return 32U;
        }
        if (score >= 16) {
            return 16U;
        }
        if (score >= 8) {
            return 8U;
        }
        return 4U;
    }
    if (mode == "turbo") {
        if (score >= 64) {
            return 512U;
        }
        if (score >= 32) {
            return 256U;
        }
        if (score >= 16) {
            return 128U;
        }
        if (score >= 8) {
            return 64U;
        }
        return 32U;
    }
    if (score >= 64) {
        return 4096U;
    }
    if (score >= 32) {
        return 2048U;
    }
    if (score >= 16) {
        return 1024U;
    }
    if (score >= 8) {
        return 512U;
    }
    return 256U;
}

static uint32_t loot_gate_salt(const QuerySpec &spec) {
    uint32_t hash = 2166136261U;
    static constexpr char kPrefix[] = "loot-gate";
    hash = fnv1a32_append_bytes(hash, kPrefix, sizeof(kPrefix) - 1U);
    for (const LootFilterSpec &lf : spec.loot_filters) {
        hash = fnv1a32_append_string(hash, lf.constraint_id);
        hash = fnv1a32_append_string(hash, lf.structure);
        hash = fnv1a32_append_u32(hash, static_cast<uint32_t>(lf.required.size()));
        for (const LootRequirementSpec &req : lf.required) {
            hash = fnv1a32_append_string(hash, req.item);
            hash = fnv1a32_append_i32(hash, static_cast<int32_t>(req.count));
        }
    }
    return hash;
}

static uint32_t strict_gate_bit_width(uint32_t gate_div) {
    uint32_t width = 0U;
    uint32_t value = std::max<uint32_t>(2U, gate_div) - 1U;
    while (value > 0U) {
        ++width;
        value >>= 1U;
    }
    return std::max<uint32_t>(1U, width);
}

static uint32_t strict_constraint_gate_salt(
    const QueryConstraintRuntime &constraint,
    uint32_t gate_div,
    uint32_t gate_order
) {
    uint32_t hash = 2166136261U;
    static constexpr char kPrefix[] = "strict-constraint-gate";
    hash = fnv1a32_append_bytes(hash, kPrefix, sizeof(kPrefix) - 1U);
    hash = fnv1a32_append_string(hash, constraint.spec.cid);
    hash = fnv1a32_append_string(hash, constraint.spec.structure);
    hash = fnv1a32_append_i32(hash, static_cast<int32_t>(constraint.dimension));
    hash = fnv1a32_append_string(hash, constraint.spec.anchor);
    hash = fnv1a32_append_i32(hash, static_cast<int32_t>(constraint.spec.radius));
    hash = fnv1a32_append_u32(hash, constraint.min_required);
    const uint32_t width = strict_gate_bit_width(gate_div);
    const uint32_t max_shift = width >= 48U ? 0U : 48U - width;
    const uint32_t shift = max_shift == 0U ? 0U : ((gate_order * width) % (max_shift + 1U));
    return (hash & 0x00FFFFFFU) | ((shift & 63U) << 24U);
}

static int estimated_sculpt_sample_count(const SculptSpec &sculpt) {
    if (!sculpt.enabled) {
        return 0;
    }
    const int radius = std::max(0, sculpt.radius);
    const int step = std::max(1, sculpt.step);
    if (sculpt.pattern == "ring") {
        if (radius == 0) {
            return 1;
        }
        const double circumference = 2.0 * 3.14159265358979323846 * static_cast<double>(radius);
        return std::max<int>(8, static_cast<int>(std::ceil(circumference / static_cast<double>(step))));
    }
    if (sculpt.pattern == "grid") {
        const int width = (radius * 2) / step + 1;
        return std::max<int>(1, width * width);
    }
    if (sculpt.pattern == "cross") {
        const int width = (radius * 2) / step + 1;
        return std::max<int>(1, (width * 2) - 1);
    }
    if (radius == 0) {
        return 1;
    }
    const double disk_points =
        (3.14159265358979323846 * static_cast<double>(radius) * static_cast<double>(radius)) /
        static_cast<double>(std::max(1, step * step));
    return std::max<int>(1, static_cast<int>(std::ceil(disk_points)));
}

static uint32_t sculpt_gate_multiplier(const QuerySpec &spec) {
    if (!spec.sculpt.enabled) {
        return 1U;
    }
    std::string mode = lower_ascii(trim(spec.perf.strict_surrogate.empty() ? std::string("off") : spec.perf.strict_surrogate));
    if (mode == "light-speed") {
        mode = "lightspeed";
    }
    uint32_t custom_gate_multiplier = 0U;
    if (parse_custom_strict_gate_multiplier(mode, custom_gate_multiplier)) {
        return std::max<uint32_t>(1U, custom_gate_multiplier);
    }
    if (mode != "aggressive" && mode != "ultra" && mode != "turbo" && mode != "lightspeed") {
        return 1U;
    }

    const int score = estimated_sculpt_sample_count(spec.sculpt);
    if (mode == "aggressive") {
        if (score >= 4000) {
            return 16U;
        }
        if (score >= 700) {
            return 8U;
        }
        if (score >= 250) {
            return 4U;
        }
        if (score >= 80) {
            return 2U;
        }
        return 1U;
    }
    if (mode == "ultra") {
        if (score >= 2500) {
            return 64U;
        }
        if (score >= 900) {
            return 32U;
        }
        if (score >= 250) {
            return 16U;
        }
        if (score >= 80) {
            return 8U;
        }
        return 4U;
    }
    if (mode == "turbo") {
        if (score >= 2500) {
            return 512U;
        }
        if (score >= 900) {
            return 256U;
        }
        if (score >= 250) {
            return 128U;
        }
        if (score >= 80) {
            return 64U;
        }
        return 32U;
    }
    if (score >= 2500) {
        return 4096U;
    }
    if (score >= 900) {
        return 2048U;
    }
    if (score >= 250) {
        return 1024U;
    }
    if (score >= 80) {
        return 512U;
    }
    return 256U;
}

static uint32_t sculpt_gate_salt(const SculptSpec &sculpt) {
    uint32_t hash = 2166136261U;
    static constexpr char kPrefix[] = "sculpt-gate";
    hash = fnv1a32_append_bytes(hash, kPrefix, sizeof(kPrefix) - 1U);
    hash = fnv1a32_append_string(hash, sculpt.pattern);
    hash = fnv1a32_append_string(hash, sculpt.edge);
    hash = fnv1a32_append_string(hash, sculpt.center);
    hash = fnv1a32_append_i32(hash, static_cast<int32_t>(sculpt.y));
    hash = fnv1a32_append_i32(hash, static_cast<int32_t>(sculpt.radius));
    hash = fnv1a32_append_i32(hash, static_cast<int32_t>(sculpt.step));
    const uint32_t dominance_ppm = static_cast<uint32_t>(std::llround(
        std::clamp(sculpt.dominance_threshold, 0.0, 1.0) * 1000000.0
    ));
    hash = fnv1a32_append_u32(hash, dominance_ppm);
    hash = fnv1a32_append_u32(hash, static_cast<uint32_t>(sculpt.allowed_ids.size()));
    for (int32_t id : sculpt.allowed_ids) {
        hash = fnv1a32_append_i32(hash, id);
    }
    return hash;
}

static inline uint32_t gpu_constraint_anchor_priority(const QueryConstraintRuntime &rc) {
    if (rc.spec.anchor == "origin") {
        return 0U;
    }
    int32_t fixed_x = 0;
    int32_t fixed_z = 0;
    return parse_fixed_anchor_coords(rc.spec.anchor, fixed_x, fixed_z) ? 0U : 1U;
}

static inline uint64_t gpu_region_center_dist2(
    const GpuRegionTerm &region,
    int32_t anchor_x,
    int32_t anchor_z
) {
    const int64_t span_blocks =
        static_cast<int64_t>(std::max<uint32_t>(1U, region.bound) - 1U) * static_cast<int64_t>(CHUNK_SIZE);
    const int64_t center_x = static_cast<int64_t>(region.base_block_x) + (span_blocks / 2LL);
    const int64_t center_z = static_cast<int64_t>(region.base_block_z) + (span_blocks / 2LL);
    const int64_t dx = center_x - static_cast<int64_t>(anchor_x);
    const int64_t dz = center_z - static_cast<int64_t>(anchor_z);
    return static_cast<uint64_t>(dx * dx + dz * dz);
}

static void sort_gpu_regions_for_single_hit_constraint(
    std::vector<GpuRegionTerm> &regions,
    int32_t anchor_x,
    int32_t anchor_z
) {
    if (regions.size() <= 1U) {
        return;
    }
    std::sort(regions.begin(), regions.end(), [&](const GpuRegionTerm &lhs, const GpuRegionTerm &rhs) {
        const uint64_t lhs_d2 = gpu_region_center_dist2(lhs, anchor_x, anchor_z);
        const uint64_t rhs_d2 = gpu_region_center_dist2(rhs, anchor_x, anchor_z);
        if (lhs_d2 != rhs_d2) {
            return lhs_d2 < rhs_d2;
        }
        if (lhs.base_block_x != rhs.base_block_x) {
            return lhs.base_block_x < rhs.base_block_x;
        }
        return lhs.base_block_z < rhs.base_block_z;
    });
}

static inline bool gpu_constraint_entry_better(
    const GpuPlanConstraintEntry &lhs,
    const GpuPlanConstraintEntry &rhs
) {
    if (lhs.anchor_priority != rhs.anchor_priority) {
        return lhs.anchor_priority < rhs.anchor_priority;
    }
    if (lhs.desc.region_count != rhs.desc.region_count) {
        return lhs.desc.region_count < rhs.desc.region_count;
    }
    if (lhs.desc.quad_max_span != rhs.desc.quad_max_span) {
        return lhs.desc.quad_max_span < rhs.desc.quad_max_span;
    }
    if (lhs.desc.radius_sq != rhs.desc.radius_sq) {
        return lhs.desc.radius_sq < rhs.desc.radius_sq;
    }
    if (lhs.desc.min_required != rhs.desc.min_required) {
        return lhs.desc.min_required > rhs.desc.min_required;
    }
    return lhs.original_order < rhs.original_order;
}

static GpuPrefilterPlan build_gpu_prefilter_plan(
    const QueryRuntime &runtime,
    const std::unordered_set<std::string> *selected_constraint_ids = nullptr
) {
    GpuPrefilterPlan plan{};
    std::vector<GpuPlanConstraintEntry> entries;
    entries.reserve(runtime.ordered_constraint_ids.size());
    uint32_t original_order = 0U;
    uint32_t strict_gate_order = 0U;

    for (const std::string &cid : runtime.ordered_constraint_ids) {
        if (selected_constraint_ids != nullptr && selected_constraint_ids->find(cid) == selected_constraint_ids->end()) {
            continue;
        }
        const QueryConstraintRuntime &rc = runtime.constraints.at(cid);
        int32_t anchor_x = 0;
        int32_t anchor_z = 0;
        if (rc.spec.structure == "stronghold") {
            continue;
        }
        if (rc.spec.anchor != "origin" && !parse_fixed_anchor_coords(rc.spec.anchor, anchor_x, anchor_z)) {
            continue;
        }
        if (!rc.prefilter_supported || rc.context.regions.empty()) {
            continue;
        }
        if (rc.spec.mode == "strict" && !rc.exact_attempt_prefilter_supported) {
            continue;
        }

        GpuPlanConstraintEntry entry{};
        entry.original_order = original_order++;
        entry.anchor_priority = gpu_constraint_anchor_priority(rc);
        entry.regions.reserve(rc.context.regions.size());

        for (const RegionTerm &region : rc.context.regions) {
            GpuRegionTerm gpu_region{};
            gpu_region.base_block_x = region.base_block_x;
            gpu_region.base_block_z = region.base_block_z;
            gpu_region.add_term_mod48 = region.add_term_mod48 & JAVA_MASK;
            gpu_region.bound = static_cast<uint32_t>(std::max(1, rc.context.preset.spacing - rc.context.preset.separation));
            gpu_region.constraint_index = 0U;
            gpu_region.spread_type = static_cast<uint32_t>(rc.context.preset.spread_type);
            gpu_region.use_fast_next_int = 0U;
            entry.regions.push_back(gpu_region);
        }

        entry.desc.region_start = 0U;
        entry.desc.region_count = static_cast<uint32_t>(entry.regions.size());
        entry.desc.radius_sq = rc.candidate_radius_sq;
        entry.desc.anchor_x = anchor_x;
        entry.desc.anchor_z = anchor_z;
        entry.desc.gate_div = 1U;
        entry.desc.gate_salt = 0U;
        entry.desc.is_gate_only = 0U;
        if (!is_exact_policy(runtime.spec) && rc.spec.mode == "strict") {
            entry.desc.gate_div = std::max<uint32_t>(
                1U,
                strict_gate_divisor(rc.spec.structure, runtime.spec.perf.strict_surrogate));
            entry.desc.gate_salt = strict_constraint_gate_salt(rc, entry.desc.gate_div, strict_gate_order++);
        }
        entry.desc.min_required = std::max<uint32_t>(1U, rc.min_required);
        entry.desc.quad_max_span = rc.quad_max_span;
        if (entry.desc.min_required == 1U) {
            sort_gpu_regions_for_single_hit_constraint(entry.regions, anchor_x, anchor_z);
        }
        entries.push_back(std::move(entry));
    }

    std::stable_sort(entries.begin(), entries.end(), [](const GpuPlanConstraintEntry &lhs, const GpuPlanConstraintEntry &rhs) {
        return gpu_constraint_entry_better(lhs, rhs);
    });

    for (GpuPlanConstraintEntry &entry : entries) {
        const uint32_t constraint_index = static_cast<uint32_t>(plan.constraints.size());
        const uint32_t region_start = static_cast<uint32_t>(plan.regions.size());
        for (GpuRegionTerm &region : entry.regions) {
            region.constraint_index = constraint_index;
            plan.regions.push_back(region);
        }
        entry.desc.region_start = region_start;
        entry.desc.region_count = static_cast<uint32_t>(plan.regions.size()) - region_start;
        plan.constraints.push_back(entry.desc);
    }

    return plan;
}

static uint64_t gpu_prefilter_gate_product(const GpuPrefilterPlan &plan) {
    uint64_t product = 1ULL;
    for (const GpuConstraintDesc &desc : plan.constraints) {
        if (desc.gate_div <= 1U) {
            continue;
        }
        const uint64_t div = static_cast<uint64_t>(desc.gate_div);
        if (product > (std::numeric_limits<uint64_t>::max() / div)) {
            return std::numeric_limits<uint64_t>::max();
        }
        product *= div;
    }
    return product;
}

static constexpr uint32_t kGpuMaxConstRegions = 2048U;

static bool gpu_prefilter_plan_fits_const_region_buffer(const GpuPrefilterPlan &plan) {
    if (plan.regions.empty() || plan.regions.size() > kGpuMaxConstRegions) {
        return false;
    }
    for (const GpuConstraintDesc &desc : plan.constraints) {
        if (desc.region_start > plan.regions.size() ||
            desc.region_count > plan.regions.size() - desc.region_start) {
            return false;
        }
    }
    return true;
}

static int64_t floor_div_i64(int64_t a, int64_t b) {
    int64_t q = a / b;
    const int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        --q;
    }
    return q;
}

static int axis_min_dist_to_point(int min_v, int max_v, int center_v) {
    if (min_v > center_v) {
        return min_v - center_v;
    }
    if (max_v < center_v) {
        return center_v - max_v;
    }
    return 0;
}

static std::vector<RegionTerm> build_candidate_regions(
    int radius_blocks,
    int spacing,
    int separation,
    int salt,
    int spread_type,
    int32_t center_x = 0,
    int32_t center_z = 0
) {
    const int bound = spacing - separation;
    const int64_t radius_sq = static_cast<int64_t>(radius_blocks) * static_cast<int64_t>(radius_blocks);
    const int span_blocks = (bound - 1) * CHUNK_SIZE;
    const int locate_offset = spread_type == SCAN_SPREAD_BURIED_TREASURE ? 9 : 0;
    const int64_t min_chunk_x = floor_div_i64(static_cast<int64_t>(center_x) - radius_blocks, CHUNK_SIZE);
    const int64_t max_chunk_x = floor_div_i64(static_cast<int64_t>(center_x) + radius_blocks, CHUNK_SIZE);
    const int64_t min_chunk_z = floor_div_i64(static_cast<int64_t>(center_z) - radius_blocks, CHUNK_SIZE);
    const int64_t max_chunk_z = floor_div_i64(static_cast<int64_t>(center_z) + radius_blocks, CHUNK_SIZE);
    const int min_rx = static_cast<int>(std::max<int64_t>(
        std::numeric_limits<int>::min(),
        floor_div_i64(min_chunk_x - (bound - 1), spacing) - 2));
    const int max_rx = static_cast<int>(std::min<int64_t>(
        std::numeric_limits<int>::max(),
        floor_div_i64(max_chunk_x, spacing) + 2));
    const int min_rz = static_cast<int>(std::max<int64_t>(
        std::numeric_limits<int>::min(),
        floor_div_i64(min_chunk_z - (bound - 1), spacing) - 2));
    const int max_rz = static_cast<int>(std::min<int64_t>(
        std::numeric_limits<int>::max(),
        floor_div_i64(max_chunk_z, spacing) + 2));

    std::vector<int> valid_rx;
    std::vector<int> valid_rz;
    for (int rx = min_rx; rx <= max_rx; ++rx) {
        const int min_cx = rx * spacing;
        const int max_cx = min_cx + bound - 1;
        if (max_cx < min_chunk_x || min_cx > max_chunk_x) {
            continue;
        }
        valid_rx.push_back(rx);
    }
    for (int rz = min_rz; rz <= max_rz; ++rz) {
        const int min_cz = rz * spacing;
        const int max_cz = min_cz + bound - 1;
        if (max_cz < min_chunk_z || min_cz > max_chunk_z) {
            continue;
        }
        valid_rz.push_back(rz);
    }

    std::vector<RegionTerm> regions;
    regions.reserve(valid_rx.size() * valid_rz.size());
    for (int rx : valid_rx) {
        for (int rz : valid_rz) {
            const int base_block_x = rx * spacing * CHUNK_SIZE;
            const int base_block_z = rz * spacing * CHUNK_SIZE;
            const int located_base_block_x = base_block_x + locate_offset;
            const int located_base_block_z = base_block_z + locate_offset;
            const int dx = axis_min_dist_to_point(located_base_block_x, located_base_block_x + span_blocks, center_x);
            const int dz = axis_min_dist_to_point(located_base_block_z, located_base_block_z + span_blocks, center_z);
            const int64_t d2 = static_cast<int64_t>(dx) * static_cast<int64_t>(dx) +
                               static_cast<int64_t>(dz) * static_cast<int64_t>(dz);
            if (d2 > radius_sq) {
                continue;
            }
            const int64_t add_term =
                (static_cast<int64_t>(salt) + static_cast<int64_t>(rx) * REGION_X_MULT +
                 static_cast<int64_t>(rz) * REGION_Z_MULT) &
                static_cast<int64_t>(JAVA_MASK);
            regions.push_back(RegionTerm{
                rx,
                rz,
                located_base_block_x,
                located_base_block_z,
                static_cast<uint64_t>(add_term),
            });
        }
    }
    return regions;
}

static int compute_candidate_cap(int radius, const StructurePreset *preset, const PerfSpec &perf) {
    if (perf.candidate_cap_mode == "none") {
        return 1'000'000'000;
    }
    if (perf.candidate_cap_mode == "fixed") {
        return std::max(1, perf.fixed_cap);
    }
    const int spacing_blocks = preset == nullptr ? CHUNK_SIZE : std::max(1, preset->spacing * CHUNK_SIZE);
    const double ratio = static_cast<double>(radius) / static_cast<double>(std::max(1, spacing_blocks));
    const int cap = static_cast<int>((ratio + 2.0) * (ratio + 2.0) * 6.0);
    return std::max(8, std::min(512, cap));
}

static std::vector<std::string> topological_constraint_order(const std::vector<ConstraintSpec> &constraints) {
    std::unordered_map<std::string, ConstraintSpec> by_id;
    for (const ConstraintSpec &c : constraints) {
        by_id.emplace(c.cid, c);
    }
    std::unordered_map<std::string, int> state;
    std::vector<std::string> order;
    order.reserve(constraints.size());

    std::function<void(const std::string &)> dfs = [&](const std::string &cid) {
        const auto st_it = state.find(cid);
        const int st = st_it == state.end() ? 0 : st_it->second;
        if (st == 1) {
            throw std::runtime_error("Constraint graph contains a cycle.");
        }
        if (st == 2) {
            return;
        }
        state[cid] = 1;
        const ConstraintSpec &c = by_id.at(cid);
        if (c.anchor.rfind("constraint:", 0) == 0) {
            dfs(c.anchor.substr(std::strlen("constraint:")));
        }
        state[cid] = 2;
        order.push_back(cid);
    };

    for (const ConstraintSpec &c : constraints) {
        dfs(c.cid);
    }
    return order;
}

static std::unordered_map<std::string, int> compute_constraint_prefilter_radii(
    const QuerySpec &spec,
    const std::vector<std::string> &order
) {
    std::unordered_map<std::string, ConstraintSpec> by_id;
    for (const ConstraintSpec &c : spec.constraints) {
        by_id.emplace(c.cid, c);
    }
    std::unordered_map<std::string, int> out;
    for (const std::string &cid : order) {
        const ConstraintSpec &c = by_id.at(cid);
        if (c.anchor == "origin") {
            out[cid] = c.radius;
        } else if (c.anchor == "spawn") {
            out[cid] = spec.perf.spawn_anchor_slack + c.radius;
        } else {
            int32_t fixed_x = 0;
            int32_t fixed_z = 0;
            if (parse_fixed_anchor_coords(c.anchor, fixed_x, fixed_z)) {
                out[cid] = c.radius;
                continue;
            }
            const std::string dep = c.anchor.substr(std::strlen("constraint:"));
            out[cid] = out.at(dep) + c.radius;
        }
    }
    return out;
}

static QueryRuntime build_query_runtime(const QuerySpec &spec) {
    QueryRuntime runtime{};
    runtime.spec = spec;
    runtime.ordered_constraint_ids = topological_constraint_order(spec.constraints);
    const auto prefilter_radii = compute_constraint_prefilter_radii(spec, runtime.ordered_constraint_ids);

    for (const ConstraintSpec &c : spec.constraints) {
        QueryConstraintRuntime rc{};
        rc.spec = c;
        rc.prefilter_radius = prefilter_radii.at(c.cid);
        rc.dimension = dimension_id_from_name(c.dimension);
        rc.candidate_radius_sq = static_cast<uint64_t>(c.radius) * static_cast<uint64_t>(c.radius);
        rc.root_radius_sq = static_cast<uint64_t>(rc.prefilter_radius) * static_cast<uint64_t>(rc.prefilter_radius);
        rc.candidate_chunk_radius = static_cast<uint32_t>(std::max(1, static_cast<int>(std::ceil(c.radius / 16.0)) + 2));
        rc.min_required = std::max<uint32_t>(1U, c.min_required);
        rc.quad_max_span = rc.min_required > 1U ? quad_max_span_from_accuracy(c.quad_accuracy) : 0U;
        {
            const auto it = kStrictSpacingHints.find(c.structure);
            const uint32_t base_hint = it == kStrictSpacingHints.end() ? 1U : it->second;
            rc.strict_region_spacing_hint =
                tuned_strict_spacing_hint(c.structure, base_hint, spec.perf.strict_surrogate);
        }
        rc.structure_in_presets = kStructurePresets.find(c.structure) != kStructurePresets.end();
        {
            const auto it = kStructureIds.find(c.structure);
            rc.struct_id = it == kStructureIds.end() ? -1 : it->second;
        }

        const StructurePreset *preset_ptr = nullptr;
        auto preset_it = kStructurePresets.find(c.structure);
        if (preset_it != kStructurePresets.end()) {
            preset_ptr = &preset_it->second;
            rc.prefilter_supported = true;
        } else if (c.mode == "strict") {
            auto sur_it = kStrictSurrogatePresets.find(c.structure);
            if (sur_it != kStrictSurrogatePresets.end()) {
                preset_ptr = &sur_it->second;
                rc.prefilter_supported = true;
            }
        }
        const StructurePreset *attempt_preset_ptr = nullptr;
        if (preset_it != kStructurePresets.end()) {
            attempt_preset_ptr = &preset_it->second;
        } else {
            const auto exact_it = kStrictSurrogatePresets.find(c.structure);
            if (exact_it != kStrictSurrogatePresets.end()) {
                attempt_preset_ptr = &exact_it->second;
            }
        }
        if (attempt_preset_ptr != nullptr && supports_exact_attempt_prefilter(c.structure)) {
            rc.exact_attempt_prefilter_supported = true;
            rc.attempt_preset = *attempt_preset_ptr;
        }
        if (rc.prefilter_supported && preset_ptr != nullptr) {
            rc.context.name = c.structure;
            rc.context.preset = *preset_ptr;
            int32_t region_center_x = 0;
            int32_t region_center_z = 0;
            (void)parse_fixed_anchor_coords(c.anchor, region_center_x, region_center_z);
            rc.context.regions = build_candidate_regions(
                rc.prefilter_radius,
                preset_ptr->spacing,
                preset_ptr->separation,
                preset_ptr->salt,
                preset_ptr->spread_type,
                region_center_x,
                region_center_z
            );
        }
        rc.candidate_cap = std::max<int>(static_cast<int>(rc.min_required), compute_candidate_cap(c.radius, preset_ptr, spec.perf));
        if (spec.perf.candidate_cap_mode == "none") {
            if (rc.prefilter_supported && !rc.context.regions.empty()) {
                const uint64_t exact_region_cap = static_cast<uint64_t>(rc.context.regions.size());
                rc.candidate_cap = static_cast<int>(std::min<uint64_t>(
                    static_cast<uint64_t>(std::numeric_limits<int>::max()),
                    std::max<uint64_t>(static_cast<uint64_t>(rc.min_required), exact_region_cap)
                ));
            } else if (c.structure == "stronghold") {
                rc.candidate_cap = std::max<int>(static_cast<int>(rc.min_required), 128);
            } else if (is_exact_policy(spec)) {
                throw std::runtime_error(
                    "Exact candidate_cap_mode=none requires an exact bounded placement preset for structure '" +
                    c.structure + "'. Set safety.completeness=approximate for capped direct scans."
                );
            }
        }

        if (c.mode == "strict" && c.structure != "stronghold" && rc.struct_id < 0) {
            throw std::runtime_error("Strict mode unsupported for structure '" + c.structure + "'.");
        }
        runtime.constraints.emplace(c.cid, std::move(rc));
    }
    return runtime;
}

static std::unordered_map<std::string, int32_t> parse_python_constant_class(
    const fs::path &py_path,
    const std::string &class_name
) {
    std::ifstream in(py_path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string line;
    bool in_class = false;
    std::unordered_map<std::string, int32_t> out;
    while (std::getline(in, line)) {
        if (!in_class) {
            const std::string needle = "class " + class_name + ":";
            if (line.find(needle) != std::string::npos) {
                in_class = true;
            }
            continue;
        }
        if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
            break;
        }
        const std::string raw = trim(line);
        if (raw.empty() || raw[0] == '#') {
            continue;
        }
        const size_t eq = raw.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string name = trim(raw.substr(0, eq));
        std::string expr = raw.substr(eq + 1);
        const size_t hash = expr.find('#');
        if (hash != std::string::npos) {
            expr = expr.substr(0, hash);
        }
        expr = trim(expr);
        if (name.empty() || expr.empty()) {
            continue;
        }

        size_t i = 0;
        auto skip_ws = [&]() {
            while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i])) != 0) {
                ++i;
            }
        };
        auto parse_term = [&]() -> int64_t {
            skip_ws();
            if (i >= expr.size()) {
                throw std::runtime_error("bad term");
            }
            if (expr[i] == '-' || (expr[i] >= '0' && expr[i] <= '9')) {
                size_t j = i + 1;
                while (j < expr.size() && expr[j] >= '0' && expr[j] <= '9') {
                    ++j;
                }
                const int64_t v = std::stoll(expr.substr(i, j - i));
                i = j;
                return v;
            }
            if ((expr[i] >= 'A' && expr[i] <= 'Z') || (expr[i] >= 'a' && expr[i] <= 'z') || expr[i] == '_') {
                size_t j = i + 1;
                while (
                    j < expr.size() &&
                    ((expr[j] >= 'A' && expr[j] <= 'Z') || (expr[j] >= 'a' && expr[j] <= 'z') ||
                     (expr[j] >= '0' && expr[j] <= '9') || expr[j] == '_')
                ) {
                    ++j;
                }
                const std::string id = expr.substr(i, j - i);
                i = j;
                const auto it = out.find(id);
                if (it == out.end()) {
                    throw std::runtime_error("unknown id");
                }
                return static_cast<int64_t>(it->second);
            }
            throw std::runtime_error("bad token");
        };

        try {
            int64_t value = parse_term();
            while (true) {
                skip_ws();
                if (i >= expr.size()) {
                    break;
                }
                const char op = expr[i++];
                if (op != '+' && op != '-') {
                    throw std::runtime_error("bad op");
                }
                const int64_t rhs = parse_term();
                if (op == '+') {
                    value += rhs;
                } else {
                    value -= rhs;
                }
            }
            if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
                continue;
            }
            out[name] = static_cast<int32_t>(value);
        } catch (...) {
            continue;
        }
    }
    return out;
}

static std::unordered_map<std::string, int32_t> parse_c_enum_named(
    const fs::path &header_path,
    const std::string &enum_name
) {
    std::ifstream in(header_path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::string stripped;
    stripped.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            if (end == std::string::npos) {
                break;
            }
            i = end + 2;
        } else if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') {
            const size_t end = text.find('\n', i + 2);
            if (end == std::string::npos) {
                break;
            }
            i = end;
        } else {
            stripped.push_back(text[i++]);
        }
    }

    const std::string needle = "enum " + enum_name;
    size_t pos = stripped.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t open = stripped.find('{', pos);
    if (open == std::string::npos) {
        return {};
    }
    const size_t close = stripped.find("};", open);
    if (close == std::string::npos) {
        return {};
    }
    const std::string body = stripped.substr(open + 1, close - open - 1);

    std::unordered_map<std::string, int32_t> out;
    int32_t next_value = 0;

    auto is_ident_start = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    auto is_ident_part = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };

    size_t i = 0;
    while (i < body.size()) {
        const size_t entry_start = i;
        while (i < body.size() && body[i] != ',') {
            ++i;
        }
        std::string entry = trim(body.substr(entry_start, i - entry_start));
        if (i < body.size()) {
            ++i;
        }
        if (entry.empty()) {
            continue;
        }

        const size_t eq = entry.find('=');
        const std::string name = trim(entry.substr(0, eq == std::string::npos ? entry.size() : eq));
        if (name.empty() || !is_ident_start(name.front())) {
            continue;
        }
        bool valid_name = true;
        for (char c : name) {
            if (!is_ident_part(c)) {
                valid_name = false;
                break;
            }
        }
        if (!valid_name) {
            continue;
        }

        int64_t value = 0;
        bool ok = true;
        if (eq == std::string::npos) {
            value = static_cast<int64_t>(next_value);
        } else {
            const std::string expr = trim(entry.substr(eq + 1));
            size_t j = 0;
            char op = '+';
            int64_t total = 0;
            while (j < expr.size()) {
                while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j])) != 0) {
                    ++j;
                }
                if (j >= expr.size()) {
                    break;
                }
                int64_t sign = 1;
                if (expr[j] == '-' || expr[j] == '+') {
                    if (expr[j] == '-') {
                        sign = -1;
                    }
                    ++j;
                    while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j])) != 0) {
                        ++j;
                    }
                }
                if (j >= expr.size()) {
                    ok = false;
                    break;
                }
                int64_t term = 0;
                if (std::isdigit(static_cast<unsigned char>(expr[j])) != 0) {
                    const size_t ts = j;
                    while (j < expr.size() && std::isdigit(static_cast<unsigned char>(expr[j])) != 0) {
                        ++j;
                    }
                    try {
                        term = std::stoll(expr.substr(ts, j - ts));
                    } catch (...) {
                        ok = false;
                        break;
                    }
                } else if (is_ident_start(expr[j])) {
                    const size_t ts = j;
                    while (j < expr.size() && is_ident_part(expr[j])) {
                        ++j;
                    }
                    const std::string id = expr.substr(ts, j - ts);
                    const auto it = out.find(id);
                    if (it == out.end()) {
                        ok = false;
                        break;
                    }
                    term = static_cast<int64_t>(it->second);
                } else {
                    ok = false;
                    break;
                }
                total = (op == '+') ? (total + sign * term) : (total - sign * term);
                while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j])) != 0) {
                    ++j;
                }
                if (j >= expr.size()) {
                    break;
                }
                if (expr[j] == '+' || expr[j] == '-') {
                    op = expr[j++];
                } else {
                    ok = false;
                    break;
                }
            }
            value = total;
        }

        if (!ok || value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
            continue;
        }
        out[name] = static_cast<int32_t>(value);
        next_value = static_cast<int32_t>(value) + 1;
    }

    return out;
}

static std::unordered_map<std::string, int32_t> build_biome_map(const fs::path &repo_root) {
    std::unordered_map<std::string, int32_t> out;
    const fs::path py_path = repo_root / "venv" / "Lib" / "site-packages" / "cubiomespi" / "cubiomes.py";
    out = parse_python_constant_class(py_path, "BiomeID");
    if (out.empty()) {
        const fs::path header_path = repo_root / "cubiomes_26.1.2_fork" / "src" / "biomes.h";
        out = parse_c_enum_named(header_path, "BiomeID");
    }
    if (out.empty()) {
        return out;
    }
    std::unordered_map<std::string, int32_t> normalized;
    normalized.reserve(out.size() * 2U);
    for (const auto &kv : out) {
        normalized[normalize_key(kv.first)] = kv.second;
    }
    normalized.emplace("pale_garden", 186);
    normalized.emplace("pale_oak_forest", 186);
    normalized.emplace("pale_forest", 186);
    return normalized;
}

static std::vector<int32_t> parse_biome_values(
    const std::vector<std::string> &tokens,
    const std::unordered_map<std::string, int32_t> &biome_map
) {
    std::vector<int32_t> out;
    std::unordered_set<int32_t> seen;
    static constexpr const char *kCaveAliasBiomeKeys[] = {
        "dripstone_caves",
        "lush_caves",
        "deep_dark",
    };
    for (const std::string &raw : tokens) {
        const std::string token = trim(raw);
        if (token.empty()) {
            continue;
        }
        int32_t value = 0;
        bool ok = false;
        try {
            const long long v = std::stoll(token, nullptr, 0);
            if (v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max()) {
                value = static_cast<int32_t>(v);
                ok = true;
            }
        } catch (...) {
        }
        if (!ok) {
            const std::string key = normalize_key(token);
            if (key == "cave" || key == "caves") {
                bool added = false;
                for (const char *biome_key : kCaveAliasBiomeKeys) {
                    const auto alias_it = biome_map.find(biome_key);
                    if (alias_it == biome_map.end()) {
                        continue;
                    }
                    if (seen.insert(alias_it->second).second) {
                        out.push_back(alias_it->second);
                    }
                    added = true;
                }
                if (!added) {
                    throw std::runtime_error(
                        "Biome alias '" + token + "' is unavailable in this cubiomes build."
                    );
                }
                continue;
            }
            const auto it = biome_map.find(key);
            if (it == biome_map.end()) {
                throw std::runtime_error("Unknown biome name/id '" + token + "'.");
            }
            value = it->second;
        }
        if (seen.insert(value).second) {
            out.push_back(value);
        }
    }
    if (out.empty()) {
        throw std::runtime_error("Biome filter must contain at least one biome.");
    }
    return out;
}

static QuerySpec parse_query_spec(
    const JsonValue &raw,
    const std::unordered_map<std::string, int32_t> &biome_map
) {
    if (raw.type != JsonValue::Type::object_t) {
        throw std::runtime_error("Query JSON must be an object.");
    }
    QuerySpec spec{};
    spec.version = json_to_int(json_get(raw, "version"), 1);
    if (spec.version != 1) {
        throw std::runtime_error("Unsupported query version.");
    }
    spec.logic = lower_ascii(trim(json_to_string(json_get(raw, "logic"), "and")));
    if (spec.logic != "and") {
        throw std::runtime_error("Only logic='and' is supported.");
    }

    const JsonValue *anchor_obj = json_get(raw, "anchor");
    if (anchor_obj != nullptr && anchor_obj->type == JsonValue::Type::object_t) {
        spec.default_anchor = parse_anchor_ref(json_to_string(json_get(*anchor_obj, "type"), "origin"));
    } else if (anchor_obj != nullptr) {
        spec.default_anchor = parse_anchor_ref(json_to_string(anchor_obj, "origin"));
    }
    spec.default_dimension = normalize_dimension_name(
        json_to_string(json_get(raw, "dimension"), json_to_string(json_get(raw, "default_dimension"), "overworld"))
    );

    const JsonValue *safety_raw = json_get(raw, "safety");
    if (safety_raw != nullptr && safety_raw->type != JsonValue::Type::null_t) {
        if (!json_is_object(safety_raw)) {
            throw std::runtime_error("safety must be an object.");
        }
        spec.safety.completeness = normalize_completeness_policy(
            json_to_string(json_get(*safety_raw, "completeness"), spec.safety.completeness)
        );
        if (spec.safety.completeness.empty()) {
            throw std::runtime_error("safety.completeness must be exact|approximate.");
        }
    }
    const JsonValue *completeness_raw = json_get(raw, "completeness");
    if (completeness_raw != nullptr) {
        spec.safety.completeness = normalize_completeness_policy(
            json_to_string(completeness_raw, spec.safety.completeness)
        );
        if (spec.safety.completeness.empty()) {
            throw std::runtime_error("completeness must be exact|approximate.");
        }
    }

    const JsonValue *control_ops_raw = json_get(raw, "control_ops");
    if (is_exact_policy(spec) && json_is_array(control_ops_raw) && !control_ops_raw->array.empty()) {
        throw std::runtime_error(
            "Exact scans do not support control_ops yet. Set safety.completeness=approximate for scripted skip/stop rules."
        );
    }

    const JsonValue *constraints_raw = json_get(raw, "constraints");
    if (constraints_raw != nullptr && !json_is_array(constraints_raw)) {
        throw std::runtime_error("constraints must be an array when provided.");
    }
    if (json_is_array(constraints_raw)) {
        std::unordered_set<std::string> ids;
        ids.reserve(constraints_raw->array.size());
        for (const JsonValue &obj : constraints_raw->array) {
            if (obj.type != JsonValue::Type::object_t) {
                throw std::runtime_error("Each constraint must be an object.");
            }
            ConstraintSpec c{};
            c.cid = trim(json_to_string(json_get(obj, "id"), ""));
            if (c.cid.empty()) {
                throw std::runtime_error("Constraint id is required.");
            }
            if (!ids.insert(c.cid).second) {
                throw std::runtime_error("Duplicate constraint id: " + c.cid);
            }
            if (is_exact_policy(spec)) {
                const JsonValue *required_raw = json_get(obj, "required");
                if (required_raw != nullptr && !json_to_bool(required_raw, true)) {
                    throw std::runtime_error(
                        "Constraint " + c.cid +
                        ": optional constraints are not part of exact native execution yet. "
                        "Set safety.completeness=approximate to use optional/script control flow."
                    );
                }
                if (json_to_bool(json_get(obj, "track_found"), false)) {
                    throw std::runtime_error(
                        "Constraint " + c.cid +
                        ": track_found/control-flow semantics are not part of exact native execution yet. "
                        "Set safety.completeness=approximate to use them."
                    );
                }
            }
            const std::string raw_structure = json_to_string(json_get(obj, "structure"), "");
            const StructureVariantResolution resolved = resolve_structure_variant(raw_structure, biome_map);
            c.structure = resolved.structure;
            c.validation_structure =
                resolved.validation_structure.empty() ? resolved.structure : resolved.validation_structure;
            c.min_required = resolved.min_required;
            c.viability_flags = resolved.viability_flags;
            c.strict_only_variant = resolved.strict_only_variant;
            c.dimension = normalize_dimension_name(json_to_string(
                json_get(obj, "dimension"),
                spec.default_dimension == "overworld" ? default_dimension_for_structure(c.structure) : spec.default_dimension
            ));
            if (!structure_dimension_compatible(c.structure, c.dimension)) {
                throw std::runtime_error(
                    "Constraint " + c.cid + ": structure '" + c.structure +
                    "' is not valid in dimension '" + c.dimension + "'."
                );
            }
            c.mode = lower_ascii(trim(json_to_string(json_get(obj, "mode"), "strict")));
            if (c.mode == "mixed") {
                c.mode = "strict";
            }
            if (c.mode != "strict" && c.mode != "placement") {
                throw std::runtime_error("Constraint " + c.cid + ": mode must be strict or placement.");
            }
            if (c.strict_only_variant && c.mode != "strict") {
                throw std::runtime_error(
                    "Constraint " + c.cid + ": structure variant '" + raw_structure + "' requires mode=strict."
                );
            }
            if (c.min_required > 1U && c.mode != "strict") {
                throw std::runtime_error("Constraint " + c.cid + ": quad structures require mode=strict.");
            }
            if (c.mode == "placement" && !supports_placement_mode(c.structure)) {
                throw std::runtime_error("Constraint " + c.cid + ": structure does not support mode=placement.");
            }
            c.structure_settings = parse_structure_settings_json(
                json_get(obj, "structure_settings"),
                "constraints[].structure_settings"
            );
            if (!c.structure_settings.empty()) {
                const std::string family = loot_family_for_structure_id(c.structure);
                if (family != "village") {
                    throw std::runtime_error(
                        "Constraint " + c.cid + ": structure_settings currently support village constraints only."
                    );
                }
                if (c.mode == "placement") {
                    throw std::runtime_error(
                        "Constraint " + c.cid +
                        ": structure_settings require strict or mixed validation; placement-only mode cannot enforce them."
                    );
                }
            }
            c.quad_accuracy = static_cast<uint32_t>(json_to_int(json_get(obj, "accuracy"), 0));
            if (c.quad_accuracy > 100U) {
                throw std::runtime_error("Constraint " + c.cid + ": accuracy must be in range 0..100.");
            }
            if (c.min_required <= 1U) {
                c.quad_accuracy = 0U;
            }
            const JsonValue *within = json_get(obj, "within");
            if (!json_is_object(within)) {
                throw std::runtime_error("Constraint " + c.cid + ": within must be an object.");
            }
            c.anchor = parse_anchor_ref(json_to_string(json_get(*within, "anchor"), spec.default_anchor));
            c.radius = json_to_int(json_get(*within, "radius"), 0);
            if (c.radius <= 0) {
                throw std::runtime_error("Constraint " + c.cid + ": radius must be > 0.");
            }
            spec.constraints.push_back(std::move(c));
        }
    }

    {
        std::unordered_map<std::string, const ConstraintSpec *> by_id;
        for (const ConstraintSpec &c : spec.constraints) {
            by_id.emplace(c.cid, &c);
        }
        for (const ConstraintSpec &c : spec.constraints) {
            if (c.anchor.rfind("constraint:", 0) == 0) {
                const std::string dep = c.anchor.substr(std::strlen("constraint:"));
                if (dep == c.cid) {
                    throw std::runtime_error("Constraint " + c.cid + ": cannot anchor to itself.");
                }
                if (by_id.find(dep) == by_id.end()) {
                    throw std::runtime_error("Constraint " + c.cid + ": unknown anchor reference.");
                }
            }
        }
        std::unordered_map<std::string, int> state;
        std::function<void(const std::string &)> dfs = [&](const std::string &cid) {
            int st = state[cid];
            if (st == 1) {
                throw std::runtime_error("Constraint graph contains a cycle.");
            }
            if (st == 2) {
                return;
            }
            state[cid] = 1;
            const ConstraintSpec &c = *by_id.at(cid);
            if (c.anchor.rfind("constraint:", 0) == 0) {
                dfs(c.anchor.substr(std::strlen("constraint:")));
            }
            state[cid] = 2;
        };
        for (const ConstraintSpec &c : spec.constraints) {
            dfs(c.cid);
        }
    }

    const JsonValue *biome_filters_raw = json_get(raw, "biome_filters");
    if (biome_filters_raw != nullptr) {
        if (!json_is_array(biome_filters_raw)) {
            throw std::runtime_error("biome_filters must be an array.");
        }
        for (const JsonValue &obj : biome_filters_raw->array) {
            if (obj.type != JsonValue::Type::object_t) {
                throw std::runtime_error("Each biome filter must be an object.");
            }
            const int bf_y = json_to_int(json_get(obj, "y"), 64);
            const int bf_radius = json_to_int(json_get(obj, "radius"), 0);
            if (bf_radius < 0) {
                throw std::runtime_error("Biome filter radius must be >= 0.");
            }
            const JsonValue *allowed = json_get(obj, "allowed");
            if (!json_is_array(allowed) || allowed->array.empty()) {
                throw std::runtime_error("Biome filter requires non-empty allowed list.");
            }
            std::vector<std::string> tokens;
            tokens.reserve(allowed->array.size());
            for (const JsonValue &v : allowed->array) {
                tokens.push_back(json_to_string(&v));
            }
            const std::vector<int32_t> allowed_ids = parse_biome_values(tokens, biome_map);

            const JsonValue *shape = json_get(obj, "shape");
            if (shape != nullptr && shape->type != JsonValue::Type::null_t) {
                if (!json_is_object(shape)) {
                    throw std::runtime_error("Biome filter shape must be an object.");
                }
                const std::string shape_type =
                    lower_ascii(trim(json_to_string(json_get(*shape, "type"), "circle")));
                if (shape_type != "circle" && shape_type != "square" && shape_type != "diamond") {
                    throw std::runtime_error("Biome filter shape.type must be circle|square|diamond.");
                }
                const std::string edge_mode = lower_ascii(trim(
                    json_to_string(
                        json_get(*shape, "edge"),
                        json_to_string(json_get(*shape, "placement"), "outward")
                    )
                ));
                if (edge_mode != "outward" && edge_mode != "inward") {
                    throw std::runtime_error("Biome filter shape.edge must be inward|outward.");
                }
                const auto [center_x, center_z] =
                    parse_shape_center_xz(json_get(*shape, "center"), "Biome filter shape.center");
                const int shape_radius = json_to_int(json_get(*shape, "radius"), 0);
                if (shape_radius < 0) {
                    throw std::runtime_error("Biome filter shape.radius must be >= 0.");
                }
                const int start_deg = json_to_int(json_get(*shape, "start_deg"), 0);
                const bool include_center = json_to_bool(json_get(*shape, "include_center"), false);
                int effective_radius = shape_radius;
                if (edge_mode == "outward") {
                    effective_radius += bf_radius;
                } else {
                    effective_radius = std::max<int>(0, shape_radius - bf_radius);
                }

                std::vector<std::string> point_anchors;
                point_anchors.reserve(128U);
                auto push_unique_anchor = [&](int32_t x, int32_t z) {
                    const std::string anchor = "fixed:" + std::to_string(x) + "," + std::to_string(z);
                    if (std::find(point_anchors.begin(), point_anchors.end(), anchor) == point_anchors.end()) {
                        point_anchors.push_back(anchor);
                    }
                };
                if (include_center) {
                    push_unique_anchor(center_x, center_z);
                }

                const int auto_step = std::max<int>(1, std::max<int>(4, bf_radius * 2));
                auto push_checked_point = [&](long long px_ll, long long pz_ll) {
                    if (
                        px_ll < static_cast<long long>(std::numeric_limits<int32_t>::min()) ||
                        px_ll > static_cast<long long>(std::numeric_limits<int32_t>::max()) ||
                        pz_ll < static_cast<long long>(std::numeric_limits<int32_t>::min()) ||
                        pz_ll > static_cast<long long>(std::numeric_limits<int32_t>::max())
                    ) {
                        throw std::runtime_error("Biome filter shape generated a point outside int32 range.");
                    }
                    push_unique_anchor(static_cast<int32_t>(px_ll), static_cast<int32_t>(pz_ll));
                };

                if (shape_type == "circle") {
                    int shape_points = json_to_int(
                        json_get(*shape, "points"),
                        json_to_int(json_get(*shape, "count"), 0)
                    );
                    if (shape_points <= 0) {
                        if (effective_radius <= 0) {
                            shape_points = 1;
                        } else {
                            const double circumference = 2.0 * 3.14159265358979323846 * static_cast<double>(effective_radius);
                            shape_points = std::max<int>(8, static_cast<int>(std::ceil(circumference / static_cast<double>(auto_step))));
                        }
                    }
                    for (int i = 0; i < shape_points; ++i) {
                        const double angle = (static_cast<double>(start_deg) +
                                              (360.0 * static_cast<double>(i) / static_cast<double>(shape_points))) *
                                             (3.14159265358979323846 / 180.0);
                        const long long px_ll = std::llround(static_cast<double>(center_x) +
                                                             static_cast<double>(effective_radius) * std::cos(angle));
                        const long long pz_ll = std::llround(static_cast<double>(center_z) +
                                                             static_cast<double>(effective_radius) * std::sin(angle));
                        push_checked_point(px_ll, pz_ll);
                    }
                } else if (shape_type == "square") {
                    const int r = effective_radius;
                    if (r <= 0) {
                        push_checked_point(center_x, center_z);
                    } else {
                        const int step = std::max<int>(1, auto_step);
                        auto emit_offset = [&](int d) {
                            push_checked_point(static_cast<long long>(center_x + d), static_cast<long long>(center_z - r));
                            push_checked_point(static_cast<long long>(center_x + d), static_cast<long long>(center_z + r));
                            push_checked_point(static_cast<long long>(center_x - r), static_cast<long long>(center_z + d));
                            push_checked_point(static_cast<long long>(center_x + r), static_cast<long long>(center_z + d));
                        };
                        for (int d = -r; d <= r; d += step) {
                            emit_offset(d);
                        }
                        if (((2 * r) % step) != 0) {
                            emit_offset(r);
                        }
                    }
                } else { // diamond
                    const int r = effective_radius;
                    if (r <= 0) {
                        push_checked_point(center_x, center_z);
                    } else {
                        const int step = std::max<int>(1, auto_step);
                        auto emit_dx = [&](int dx) {
                            const int rem = r - std::abs(dx);
                            push_checked_point(static_cast<long long>(center_x + dx), static_cast<long long>(center_z + rem));
                            push_checked_point(static_cast<long long>(center_x + dx), static_cast<long long>(center_z - rem));
                        };
                        for (int dx = -r; dx <= r; dx += step) {
                            emit_dx(dx);
                        }
                        if (((2 * r) % step) != 0) {
                            emit_dx(r);
                        }
                    }
                }
                for (const std::string &point_anchor : point_anchors) {
                    BiomeFilterSpec bf{};
                    bf.point = parse_anchor_ref(point_anchor);
                    bf.dimension = normalize_dimension_name(json_to_string(json_get(obj, "dimension"), spec.default_dimension));
                    if (bf.point == "spawn" && bf.dimension != "overworld") {
                        throw std::runtime_error("Biome filter point=spawn only supports the overworld dimension.");
                    }
                    bf.y = bf_y;
                    bf.radius = bf_radius;
                    bf.allowed_ids = allowed_ids;
                    spec.biome_filters.push_back(std::move(bf));
                }
                continue;
            }

            BiomeFilterSpec bf{};
            bf.point = parse_anchor_ref(json_to_string(json_get(obj, "point"), spec.default_anchor));
            bf.dimension = normalize_dimension_name(json_to_string(json_get(obj, "dimension"), spec.default_dimension));
            if (bf.point == "spawn" && bf.dimension != "overworld") {
                throw std::runtime_error("Biome filter point=spawn only supports the overworld dimension.");
            }
            bf.y = bf_y;
            bf.radius = bf_radius;
            bf.allowed_ids = allowed_ids;
            spec.biome_filters.push_back(std::move(bf));
        }
    }

    const JsonValue *loot_filters_raw = json_get(raw, "loot_filters");
    if (loot_filters_raw != nullptr) {
        if (!json_is_array(loot_filters_raw)) {
            throw std::runtime_error("loot_filters must be an array.");
        }
        std::unordered_map<std::string, const ConstraintSpec *> by_id;
        by_id.reserve(spec.constraints.size());
        for (const ConstraintSpec &c : spec.constraints) {
            by_id.emplace(c.cid, &c);
        }
        for (const JsonValue &obj : loot_filters_raw->array) {
            if (obj.type != JsonValue::Type::object_t) {
                throw std::runtime_error("Each loot filter must be an object.");
            }
            LootFilterSpec lf{};
            lf.constraint_id = trim(json_to_string(json_get(obj, "constraint"), json_to_string(json_get(obj, "source"), "")));
            if (lf.constraint_id.empty()) {
                throw std::runtime_error("loot_filters[].constraint is required.");
            }
            const auto constraint_it = by_id.find(lf.constraint_id);
            if (constraint_it == by_id.end()) {
                throw std::runtime_error("loot_filters references unknown constraint id '" + lf.constraint_id + "'.");
            }
            const ConstraintSpec &constraint = *constraint_it->second;
            lf.structure = normalize_loot_structure_id(json_to_string(json_get(obj, "structure"), "auto"));
            if (lf.structure.empty()) {
                lf.structure = "auto";
            }
            if (lf.structure != "auto" && !loot_source_matches_constraint(lf.structure, constraint.structure)) {
                throw std::runtime_error(
                    "loot_filters[].structure must be auto or belong to the referenced structure family."
                );
            }
            const JsonValue *required_raw = json_get(obj, "required");
            if (required_raw == nullptr) {
                required_raw = json_get(obj, "items");
            }
            lf.required = parse_loot_requirements_json(required_raw, "loot_filters[].required");
            spec.loot_filters.push_back(std::move(lf));
        }
    }

    const JsonValue *sculpt_raw = json_get(raw, "sculpt");
    if (sculpt_raw != nullptr && sculpt_raw->type != JsonValue::Type::null_t) {
        if (!json_is_object(sculpt_raw)) {
            throw std::runtime_error("sculpt must be an object.");
        }
        SculptSpec sculpt{};
        sculpt.enabled = json_to_bool(json_get(*sculpt_raw, "enabled"), false);
        if (sculpt.enabled) {
            sculpt.pattern = normalize_sculpt_pattern(json_to_string(json_get(*sculpt_raw, "pattern"), "disk"));
            if (sculpt.pattern.empty()) {
                throw std::runtime_error("sculpt.pattern must be disk|ring|grid|cross.");
            }
            sculpt.edge = lower_ascii(trim(json_to_string(json_get(*sculpt_raw, "edge"), "outward")));
            if (sculpt.edge != "outward" && sculpt.edge != "inward") {
                throw std::runtime_error("sculpt.edge must be outward|inward.");
            }
            sculpt.center = parse_anchor_ref(json_to_string(json_get(*sculpt_raw, "center"), "origin"));
            if (sculpt.center.rfind("constraint:", 0) == 0) {
                throw std::runtime_error("sculpt.center cannot use constraint:<id> in this build.");
            }
            sculpt.dimension = normalize_dimension_name(json_to_string(json_get(*sculpt_raw, "dimension"), spec.default_dimension));
            if (sculpt.center == "spawn" && sculpt.dimension != "overworld") {
                throw std::runtime_error("sculpt.center=spawn only supports the overworld dimension.");
            }
            sculpt.y = json_to_int(json_get(*sculpt_raw, "y"), 64);
            sculpt.radius = json_to_int(json_get(*sculpt_raw, "radius"), 128);
            if (sculpt.radius < 0) {
                throw std::runtime_error("sculpt.radius must be >= 0.");
            }
            sculpt.step = json_to_int(json_get(*sculpt_raw, "step"), 16);
            if (sculpt.step <= 0) {
                throw std::runtime_error("sculpt.step must be > 0.");
            }
            sculpt.dominance_threshold = json_to_double(json_get(*sculpt_raw, "dominance_threshold"), 0.85);
            if (!std::isfinite(sculpt.dominance_threshold) || sculpt.dominance_threshold < 0.0 ||
                sculpt.dominance_threshold > 1.0) {
                throw std::runtime_error("sculpt.dominance_threshold must be within [0, 1].");
            }
            const JsonValue *allowed = json_get(*sculpt_raw, "allowed");
            if (!json_is_array(allowed) || allowed->array.empty()) {
                throw std::runtime_error("sculpt.allowed must be a non-empty array when sculpt.enabled=true.");
            }
            std::vector<std::string> tokens;
            tokens.reserve(allowed->array.size());
            for (const JsonValue &v : allowed->array) {
                tokens.push_back(json_to_string(&v));
            }
            sculpt.allowed_ids = parse_biome_values(tokens, biome_map);
        }
        spec.sculpt = std::move(sculpt);
    }

    const JsonValue *perf = json_get(raw, "performance");
    if (perf != nullptr) {
        if (!json_is_object(perf)) {
            throw std::runtime_error("performance must be an object.");
        }
        spec.perf.candidate_cap_mode = lower_ascii(trim(json_to_string(
            json_get(*perf, "candidate_cap_mode"),
            spec.perf.candidate_cap_mode
        )));
        if (
            spec.perf.candidate_cap_mode != "adaptive" && spec.perf.candidate_cap_mode != "fixed" &&
            spec.perf.candidate_cap_mode != "none"
        ) {
            throw std::runtime_error("candidate_cap_mode must be adaptive|fixed|none.");
        }
        spec.perf.fixed_cap = json_to_int(json_get(*perf, "fixed_cap"), 128);
        if (spec.perf.fixed_cap <= 0) {
            throw std::runtime_error("performance.fixed_cap must be > 0.");
        }
        spec.perf.strict_workers = json_to_int(json_get(*perf, "strict_workers"), 0);
        spec.perf.spawn_anchor_slack = json_to_int(json_get(*perf, "spawn_anchor_slack"), 1024);
        if (spec.perf.spawn_anchor_slack < 0) {
            throw std::runtime_error("performance.spawn_anchor_slack must be >= 0.");
        }
        spec.perf.strict_surrogate = lower_ascii(trim(json_to_string(
            json_get(*perf, "strict_surrogate"),
            spec.perf.strict_surrogate
        )));
        if (!is_supported_strict_surrogate_mode(spec.perf.strict_surrogate)) {
            throw std::runtime_error(
                "performance.strict_surrogate must be off|balanced|aggressive|ultra|turbo|lightspeed|gate:<n>."
            );
        }
        spec.perf.execution_mode =
            normalize_execution_mode(json_to_string(json_get(*perf, "execution_mode"), "auto"));
        if (spec.perf.execution_mode.empty()) {
            throw std::runtime_error(
                "performance.execution_mode must be auto|scalar|simd-x4|simd-x8|max-throughput."
            );
        }
        spec.perf.compiled_stage_a_mode = normalize_compiled_stage_a_mode(
            json_to_string(json_get(*perf, "compiled_stage_a_mode"), spec.perf.compiled_stage_a_mode)
        );
        if (spec.perf.compiled_stage_a_mode.empty()) {
            throw std::runtime_error(
                "performance.compiled_stage_a_mode must be auto|single|multi|cpu-a5|gpu-off."
            );
        }
    }

    if (spec.constraints.empty() && spec.biome_filters.empty() && !spec.sculpt.enabled) {
        throw std::runtime_error("Query requires at least one constraint, biome filter, or sculpt block.");
    }

    return spec;
}

static std::tuple<std::vector<NativeRegionTerm>, std::vector<NativeQueryConstraintDesc>, std::unordered_map<std::string, int>>
build_native_query_arrays(const QueryRuntime &runtime) {
    std::vector<NativeRegionTerm> native_regions;
    std::vector<NativeQueryConstraintDesc> native_constraints;
    std::unordered_map<std::string, int> constraint_index;
    constraint_index.reserve(runtime.ordered_constraint_ids.size());
    for (size_t i = 0; i < runtime.ordered_constraint_ids.size(); ++i) {
        constraint_index.emplace(runtime.ordered_constraint_ids[i], static_cast<int>(i));
    }

    for (const std::string &cid : runtime.ordered_constraint_ids) {
        const QueryConstraintRuntime &rc = runtime.constraints.at(cid);
        const uint32_t region_start = static_cast<uint32_t>(native_regions.size());
        if (rc.prefilter_supported) {
            const int bound = std::max(1, rc.context.preset.spacing - rc.context.preset.separation);
            for (const RegionTerm &region : rc.context.regions) {
                NativeRegionTerm term{};
                term.rx = region.rx;
                term.rz = region.rz;
                term.base_block_x = region.base_block_x;
                term.base_block_z = region.base_block_z;
                term.add_term_mod48 = region.add_term_mod48 & JAVA_MASK;
                term.bound = static_cast<uint32_t>(bound);
                term.spread_type = static_cast<uint32_t>(rc.context.preset.spread_type);
                term.structure_in_presets = rc.structure_in_presets ? 1U : 0U;
                native_regions.push_back(term);
            }
        }
        const uint32_t region_count = static_cast<uint32_t>(native_regions.size()) - region_start;

        uint32_t anchor_type = SCAN_ANCHOR_ORIGIN;
        int32_t anchor_dep_index = -1;
        int32_t anchor_x = 0;
        int32_t anchor_z = 0;
        if (rc.spec.anchor == "spawn") {
            anchor_type = SCAN_ANCHOR_SPAWN;
        } else if (rc.spec.anchor.rfind("constraint:", 0) == 0) {
            anchor_type = SCAN_ANCHOR_DEP;
            anchor_dep_index = constraint_index.at(rc.spec.anchor.substr(std::strlen("constraint:")));
        } else if (parse_fixed_anchor_coords(rc.spec.anchor, anchor_x, anchor_z)) {
            anchor_type = SCAN_ANCHOR_FIXED;
        }

        NativeQueryConstraintDesc desc{};
        desc.region_start = region_start;
        desc.region_count = region_count;
        desc.candidate_radius_sq = rc.candidate_radius_sq;
        desc.mode_strict = rc.spec.mode == "strict" ? 1U : 0U;
        desc.anchor_type = anchor_type;
        desc.anchor_dep_index = anchor_dep_index;
        desc.candidate_cap = static_cast<uint32_t>(std::max(1, rc.candidate_cap));
        desc.candidate_chunk_radius = rc.candidate_chunk_radius;
        desc.prefilter_supported = rc.prefilter_supported ? 1U : 0U;
        desc.region_spacing_chunks =
            rc.prefilter_supported ? static_cast<uint32_t>(std::max(1, rc.context.preset.spacing)) : 0U;
        desc.strict_region_spacing_hint = rc.strict_region_spacing_hint;
        desc.dimension = rc.dimension;
        desc.struct_id = rc.struct_id;
        desc.is_stronghold = rc.spec.structure == "stronghold" ? 1U : 0U;
        desc.structure_in_presets = rc.structure_in_presets ? 1U : 0U;
        desc.anchor_x = anchor_x;
        desc.anchor_z = anchor_z;
        desc.min_required = rc.min_required;
        desc.quad_max_span = rc.quad_max_span;
        desc.exact_attempt_prefilter = rc.exact_attempt_prefilter_supported ? 1U : 0U;
        if (rc.exact_attempt_prefilter_supported) {
            desc.attempt_bound =
                static_cast<uint32_t>(std::max(1, rc.attempt_preset.spacing - rc.attempt_preset.separation));
            desc.attempt_spread_type = static_cast<uint32_t>(rc.attempt_preset.spread_type);
            desc.attempt_salt = rc.attempt_preset.salt;
        }
        desc.attempt_anchor_exact_stage0 =
            (rc.prefilter_supported && rc.spec.structure != "stronghold" &&
             (rc.spec.mode != "strict" || rc.exact_attempt_prefilter_supported))
                ? 1U
                : 0U;
        desc.viability_flags = rc.spec.viability_flags;
        native_constraints.push_back(desc);
    }
    return {native_regions, native_constraints, constraint_index};
}

static std::pair<std::vector<NativeBiomeFilterDesc>, std::vector<int32_t>> build_native_query_biome_arrays(
    const QuerySpec &spec,
    const std::unordered_map<std::string, int> &constraint_index
) {
    std::vector<NativeBiomeFilterDesc> filters;
    std::vector<int32_t> allowed_values;
    for (const BiomeFilterSpec &bf : spec.biome_filters) {
        NativeBiomeFilterDesc out{};
        int32_t point_x = 0;
        int32_t point_z = 0;
        if (bf.point == "origin") {
            out.point_type = SCAN_ANCHOR_ORIGIN;
            out.point_dep_index = -1;
        } else if (bf.point == "spawn") {
            out.point_type = SCAN_ANCHOR_SPAWN;
            out.point_dep_index = -1;
        } else if (parse_fixed_anchor_coords(bf.point, point_x, point_z)) {
            out.point_type = SCAN_ANCHOR_FIXED;
            out.point_dep_index = -1;
        } else {
            out.point_type = SCAN_ANCHOR_DEP;
            out.point_dep_index = constraint_index.at(bf.point.substr(std::strlen("constraint:")));
        }
        out.dimension = dimension_id_from_name(bf.dimension);
        out.y = bf.y;
        out.radius = bf.radius;
        out.sample_step = static_cast<uint32_t>(effective_biome_sample_step(spec.perf.strict_surrogate, bf.radius));
        out.allowed_start = static_cast<uint32_t>(allowed_values.size());
        out.allowed_count = static_cast<uint32_t>(bf.allowed_ids.size());
        for (int32_t id : bf.allowed_ids) {
            allowed_values.push_back(id);
        }
        out.point_x = point_x;
        out.point_z = point_z;
        filters.push_back(out);
    }
    return {filters, allowed_values};
}

static std::pair<NativeSculptDesc, std::vector<int32_t>> build_native_sculpt_config(const QuerySpec &spec) {
    NativeSculptDesc out{};
    std::vector<int32_t> allowed;
    if (!spec.sculpt.enabled) {
        return {out, allowed};
    }

    out.enabled = 1U;
    if (spec.sculpt.pattern == "disk") {
        out.pattern = SCAN_SCULPT_DISK;
    } else if (spec.sculpt.pattern == "ring") {
        out.pattern = SCAN_SCULPT_RING;
    } else if (spec.sculpt.pattern == "grid") {
        out.pattern = SCAN_SCULPT_GRID;
    } else if (spec.sculpt.pattern == "cross") {
        out.pattern = SCAN_SCULPT_CROSS;
    } else {
        throw std::runtime_error("Unsupported sculpt.pattern: " + spec.sculpt.pattern);
    }
    if (spec.sculpt.edge == "outward") {
        out.edge_mode = SCAN_EDGE_OUTWARD;
    } else if (spec.sculpt.edge == "inward") {
        out.edge_mode = SCAN_EDGE_INWARD;
    } else {
        throw std::runtime_error("Unsupported sculpt.edge: " + spec.sculpt.edge);
    }

    int32_t center_x = 0;
    int32_t center_z = 0;
    if (spec.sculpt.center == "origin") {
        out.center_type = SCAN_ANCHOR_ORIGIN;
    } else if (spec.sculpt.center == "spawn") {
        out.center_type = SCAN_ANCHOR_SPAWN;
    } else if (parse_fixed_anchor_coords(spec.sculpt.center, center_x, center_z)) {
        out.center_type = SCAN_ANCHOR_FIXED;
    } else {
        throw std::runtime_error("sculpt.center must be origin|spawn|fixed:x,z.");
    }

    out.center_x = center_x;
    out.center_z = center_z;
    out.dimension = dimension_id_from_name(spec.sculpt.dimension);
    out.y = spec.sculpt.y;
    out.radius = std::max<int32_t>(0, spec.sculpt.radius);
    out.step = std::max<int32_t>(1, spec.sculpt.step);
    out.dominance_threshold = spec.sculpt.dominance_threshold;

    allowed = spec.sculpt.allowed_ids;
    if (allowed.empty()) {
        throw std::runtime_error("sculpt.allowed must contain at least one biome.");
    }
    return {out, allowed};
}

static std::string infer_auto_loot_structure_for_constraint(
    const std::string &version_token,
    const QueryConstraintRuntime &constraint
) {
    const std::string base = normalize_loot_structure_id(constraint.spec.structure);
    if (is_supported_loot_structure_for_version(version_token, base)) {
        return base;
    }

    if (loot_family_for_structure_id(base) == "village") {
        const auto buildings_it = constraint.spec.structure_settings.find("buildings");
        if (buildings_it != constraint.spec.structure_settings.end()) {
            std::vector<std::string> supported_buildings;
            for (const std::string &building : buildings_it->second) {
                const std::string source = normalize_loot_structure_id(building);
                if (!is_supported_loot_structure_for_version(version_token, source)) {
                    continue;
                }
                if (std::find(supported_buildings.begin(), supported_buildings.end(), source) ==
                    supported_buildings.end()) {
                    supported_buildings.push_back(source);
                }
            }
            if (supported_buildings.size() == 1U) {
                return supported_buildings.front();
            }
        }
    }

    return base;
}

static std::vector<PreparedLootFilter> prepare_loot_filters(
    const QueryRuntime &runtime,
    const std::unordered_map<std::string, int> &constraint_index,
    const std::string &version_token
) {
    if (runtime.spec.loot_filters.empty()) {
        return {};
    }
    if (!is_supported_loot_version_token(version_token)) {
        throw std::runtime_error(
            "loot_filters currently support mc-version 26.1.1 / 26.1.2 exactly, or legacy 1.17 / 1.17.1."
        );
    }

    std::vector<PreparedLootFilter> out;
    out.reserve(runtime.spec.loot_filters.size());
    for (const LootFilterSpec &lf : runtime.spec.loot_filters) {
        const auto idx_it = constraint_index.find(lf.constraint_id);
        if (idx_it == constraint_index.end()) {
            throw std::runtime_error("loot_filters references unknown constraint id '" + lf.constraint_id + "'.");
        }
        const auto runtime_it = runtime.constraints.find(lf.constraint_id);
        if (runtime_it == runtime.constraints.end()) {
            throw std::runtime_error("Missing runtime data for loot filter constraint '" + lf.constraint_id + "'.");
        }
        const QueryConstraintRuntime &constraint = runtime_it->second;

        std::string effective_structure = lf.structure.empty() || lf.structure == "auto"
            ? infer_auto_loot_structure_for_constraint(version_token, constraint)
            : lf.structure;
        effective_structure = normalize_loot_structure_id(std::move(effective_structure));
        if (!loot_source_matches_constraint(effective_structure, constraint.spec.structure)) {
            throw std::runtime_error(
                "loot_filters structure '" + effective_structure + "' does not belong to referenced constraint '" +
                lf.constraint_id + "' (" + constraint.spec.structure + ")."
            );
        }
        if (!is_supported_loot_structure_for_version(version_token, effective_structure)) {
            throw std::runtime_error(
                "Unsupported loot filter structure '" + effective_structure + "' for mc-version " + trim(version_token) +
                ". Exact 26.1.1 / 26.1.2 currently supports buried_treasure, ruined_portal, and blacksmith/village_weaponsmith."
            );
        }

        PreparedLootFilter prepared{};
        prepared.constraint_index = static_cast<uint32_t>(idx_it->second);
        prepared.constraint_id = lf.constraint_id;
        prepared.query_structure = constraint.spec.structure;
        prepared.effective_structure = effective_structure;
        prepared.required.reserve(lf.required.size());
        for (const LootRequirementSpec &req : lf.required) {
            prepared.required.push_back(PreparedLootRequirement{normalize_loot_item_id(req.item), req.count});
        }
        prepared.encoded_required = encode_loot_requirements_for_server(prepared.required);
        auto merge_it = std::find_if(out.begin(), out.end(), [&](const PreparedLootFilter &existing) {
            return existing.constraint_index == prepared.constraint_index &&
                   existing.constraint_id == prepared.constraint_id &&
                   existing.query_structure == prepared.query_structure &&
                   existing.effective_structure == prepared.effective_structure;
        });
        if (merge_it == out.end()) {
            out.push_back(std::move(prepared));
            continue;
        }
        for (const PreparedLootRequirement &req : prepared.required) {
            auto req_it = std::find_if(
                merge_it->required.begin(),
                merge_it->required.end(),
                [&](const PreparedLootRequirement &existing) { return existing.item == req.item; });
            if (req_it == merge_it->required.end()) {
                merge_it->required.push_back(req);
            } else {
                req_it->count = std::max(req_it->count, req.count);
            }
        }
        merge_it->encoded_required = encode_loot_requirements_for_server(merge_it->required);
    }
    return out;
}

static bool is_exact_java_cubiomes_validation_version(const std::string &version_token) {
    const std::string token = lower_ascii(trim(version_token));
    return token == "26.1.2" || token == "26.1.1" || token == "1.21.11" || token == "4790";
}

static std::vector<PreparedStructureSettingsFilter> prepare_structure_settings_filters(
    const QueryRuntime &runtime,
    const std::unordered_map<std::string, int> &constraint_index
) {
    std::vector<PreparedStructureSettingsFilter> out;
    out.reserve(runtime.ordered_constraint_ids.size());
    for (const std::string &cid : runtime.ordered_constraint_ids) {
        const auto runtime_it = runtime.constraints.find(cid);
        const auto idx_it = constraint_index.find(cid);
        if (runtime_it == runtime.constraints.end() || idx_it == constraint_index.end()) {
            continue;
        }
        const QueryConstraintRuntime &constraint = runtime_it->second;
        if (constraint.spec.structure_settings.empty()) {
            continue;
        }
        if (loot_family_for_structure_id(constraint.spec.structure) != "village") {
            throw std::runtime_error(
                "structure_settings currently support village constraints only: " + constraint.spec.cid
            );
        }
        PreparedStructureSettingsFilter prepared{};
        prepared.constraint_index = static_cast<uint32_t>(idx_it->second);
        prepared.constraint_id = constraint.spec.cid;
        prepared.structure = normalize_loot_structure_id(constraint.spec.structure);
        prepared.encoded_settings = encode_structure_settings_for_server(constraint.spec.structure_settings);
        if (prepared.encoded_settings != "_") {
            out.push_back(std::move(prepared));
        }
    }
    return out;
}

static std::vector<PreparedJavaStructureValidationConstraint> prepare_java_structure_validation_constraints(
    const QueryRuntime &runtime,
    const std::unordered_map<std::string, int> &constraint_index
) {
    std::vector<PreparedJavaStructureValidationConstraint> out;
    out.reserve(runtime.ordered_constraint_ids.size());
    for (const std::string &cid : runtime.ordered_constraint_ids) {
        const auto idx_it = constraint_index.find(cid);
        const auto runtime_it = runtime.constraints.find(cid);
        if (idx_it == constraint_index.end() || runtime_it == runtime.constraints.end()) {
            continue;
        }
        const QueryConstraintRuntime &constraint = runtime_it->second;
        if (constraint.spec.mode != "strict") {
            continue;
        }
        PreparedJavaStructureValidationConstraint prepared{};
        prepared.constraint_index = static_cast<uint32_t>(idx_it->second);
        prepared.constraint_id = cid;
        prepared.structure = constraint.spec.validation_structure.empty()
                                 ? constraint.spec.structure
                                 : constraint.spec.validation_structure;
        prepared.min_required = std::max<uint32_t>(1U, constraint.min_required);
        out.push_back(std::move(prepared));
    }
    return out;
}

static uint32_t initial_query_match_cap(const std::vector<NativeQueryConstraintDesc> &constraints) {
    uint64_t total = 0ULL;
    for (const NativeQueryConstraintDesc &desc : constraints) {
        total += std::max<uint32_t>(1U, desc.candidate_cap);
    }
    total = std::max<uint64_t>(1ULL, std::min<uint64_t>(total, static_cast<uint64_t>(1U << 20U)));
    return static_cast<uint32_t>(total);
}

static uint64_t pack_match_position_key(int32_t x, int32_t z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32U) |
           static_cast<uint64_t>(static_cast<uint32_t>(z));
}

static uint32_t native_java_next_int_exact(uint64_t &state48, uint32_t bound) {
    bound = std::max<uint32_t>(1U, bound);
    if ((bound & (bound - 1U)) == 0U) {
        state48 = (state48 * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
        const uint32_t bits = static_cast<uint32_t>(state48 >> 17U);
        return static_cast<uint32_t>((static_cast<uint64_t>(bound) * bits) >> 31U);
    }
    while (true) {
        state48 = (state48 * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
        const uint32_t bits = static_cast<uint32_t>(state48 >> 17U);
        const uint32_t val = bits % bound;
        if ((bits - val + (bound - 1U)) < INT32_SIGN) {
            return val;
        }
    }
}

static bool native_buried_treasure_frequency_pass(uint64_t seed, const NativeRegionTerm &region) {
    uint64_t state = ((seed + (region.add_term_mod48 & JAVA_MASK)) ^ JAVA_MULT) & JAVA_MASK;
    state = (state * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
    const uint32_t bits24 = static_cast<uint32_t>(state >> 24U);
    return bits24 < 167773U;
}

static void native_structure_attempt_block_pos(
    uint64_t seed,
    const NativeRegionTerm &region,
    int32_t &out_x,
    int32_t &out_z
) {
    if (region.spread_type == SCAN_SPREAD_BURIED_TREASURE) {
        out_x = region.base_block_x;
        out_z = region.base_block_z;
        return;
    }

    const uint32_t bound = std::max<uint32_t>(1U, region.bound);
    uint64_t state = ((seed + (region.add_term_mod48 & JAVA_MASK)) ^ JAVA_MULT) & JAVA_MASK;
    uint32_t off_x = 0U;
    uint32_t off_z = 0U;
    if (region.spread_type == SCAN_SPREAD_TRIANGULAR) {
        const uint32_t x1 = native_java_next_int_exact(state, bound);
        const uint32_t x2 = native_java_next_int_exact(state, bound);
        const uint32_t z1 = native_java_next_int_exact(state, bound);
        const uint32_t z2 = native_java_next_int_exact(state, bound);
        off_x = (x1 + x2) / 2U;
        off_z = (z1 + z2) / 2U;
    } else {
        off_x = native_java_next_int_exact(state, bound);
        off_z = native_java_next_int_exact(state, bound);
    }

    out_x = region.base_block_x + static_cast<int32_t>(off_x * static_cast<uint32_t>(CHUNK_SIZE));
    out_z = region.base_block_z + static_cast<int32_t>(off_z * static_cast<uint32_t>(CHUNK_SIZE));
}

static uint64_t native_dist2_i64(int32_t x0, int32_t z0, int32_t x1, int32_t z1) {
    const int64_t dx = static_cast<int64_t>(x0) - static_cast<int64_t>(x1);
    const int64_t dz = static_cast<int64_t>(z0) - static_cast<int64_t>(z1);
    return static_cast<uint64_t>(dx * dx + dz * dz);
}

static bool try_collect_fast_loot_matches_for_filter(
    uint64_t seed,
    const PreparedLootFilter &filter,
    const CompiledQueryPlanContext &query_plan,
    std::vector<NativeQueryMatchOut> &match_buffer,
    uint32_t &match_count
) {
    match_count = 0U;
    if (filter.constraint_index >= query_plan.constraints.size()) {
        return false;
    }
    const NativeQueryConstraintDesc &desc = query_plan.constraints[filter.constraint_index];
    if (desc.prefilter_supported == 0U || desc.region_count == 0U ||
        (desc.anchor_type != SCAN_ANCHOR_ORIGIN && desc.anchor_type != SCAN_ANCHOR_FIXED)) {
        return false;
    }

    const int32_t anchor_x = desc.anchor_type == SCAN_ANCHOR_FIXED ? desc.anchor_x : 0;
    const int32_t anchor_z = desc.anchor_type == SCAN_ANCHOR_FIXED ? desc.anchor_z : 0;
    const uint32_t region_end = desc.region_start + desc.region_count;
    if (region_end > query_plan.regions.size()) {
        return false;
    }

    std::unordered_set<uint64_t> seen_positions;
    seen_positions.reserve(std::min<uint32_t>(desc.region_count, std::max<uint32_t>(1U, desc.candidate_cap)));
    for (uint32_t i = desc.region_start; i < region_end; ++i) {
        const NativeRegionTerm &region = query_plan.regions[i];
        if (region.spread_type == SCAN_SPREAD_BURIED_TREASURE &&
            !native_buried_treasure_frequency_pass(seed, region)) {
            continue;
        }
        int32_t x = 0;
        int32_t z = 0;
        native_structure_attempt_block_pos(seed, region, x, z);
        const uint64_t dist2 = native_dist2_i64(x, z, anchor_x, anchor_z);
        if (dist2 > desc.candidate_radius_sq) {
            continue;
        }
        const uint64_t packed = pack_match_position_key(x, z);
        if (!seen_positions.insert(packed).second) {
            continue;
        }
        if (match_count >= match_buffer.size()) {
            match_buffer.resize(static_cast<size_t>(match_count) + 16U);
        }
        match_buffer[match_count++] = NativeQueryMatchOut{
            filter.constraint_index,
            x,
            z,
            anchor_x,
            anchor_z,
            dist2,
        };
        if (match_count >= std::max<uint32_t>(1U, desc.candidate_cap)) {
            break;
        }
    }
    return true;
}

static std::string resolve_effective_loot_roll_structure(
    const PreparedLootFilter &filter,
    const std::string &version_token,
    uint64_t seed,
    int32_t cubi_mc_version,
    int32_t match_x,
    int32_t match_z,
    std::unordered_map<uint64_t, bool> &exact_shipwreck_variant_cache
) {
    std::string roll_structure = filter.effective_structure;
    if (is_exact_loot_version_token(version_token) && is_ruined_portal_loot_structure_id(filter.effective_structure)) {
        const std::string query_structure = normalize_loot_structure_id(filter.query_structure);
        if (is_ruined_portal_loot_structure_id(query_structure)) {
            roll_structure = query_structure;
        }
    }
    if (is_exact_loot_version_token(version_token) && is_shipwreck_loot_structure_id(filter.effective_structure)) {
        const uint64_t packed = pack_match_position_key(match_x, match_z);
        const auto beached_it = exact_shipwreck_variant_cache.find(packed);
        const bool is_beached = beached_it != exact_shipwreck_variant_cache.end()
            ? beached_it->second
            : detect_exact_shipwreck_beached(seed, cubi_mc_version, match_x, match_z);
        if (beached_it == exact_shipwreck_variant_cache.end()) {
            exact_shipwreck_variant_cache.emplace(packed, is_beached);
        }
        roll_structure = exact_shipwreck_roll_structure_id(filter.effective_structure, is_beached);
    }
    return roll_structure;
}

static std::vector<NativeQueryMatchOut> collect_query_match_details_for_seed(
    uint64_t seed,
    const CompiledQueryPlanContext &query_plan
) {
    std::vector<NativeQueryMatchOut> match_buffer(std::max<uint32_t>(1U, initial_query_match_cap(query_plan.constraints)));
    uint32_t match_count = 0U;
    uint32_t biome_count = 0U;
    uint32_t cap_pruned = 0U;

    while (true) {
        const int rc = query_plan.collect_details(
            seed,
            match_buffer.empty() ? nullptr : match_buffer.data(),
            static_cast<uint32_t>(match_buffer.size()),
            &match_count,
            nullptr,
            0U,
            &biome_count,
            &cap_pruned,
            false
        );
        if (rc < 0) {
            throw std::runtime_error("scanner_core_collect_query_details failed with code " + fmt_i64(rc));
        }
        if (rc != 0) {
            return {};
        }
        if (match_count <= match_buffer.size()) {
            match_buffer.resize(match_count);
            return match_buffer;
        }
        match_buffer.resize(match_count);
    }
}

static bool validate_java_structure_matches_for_seed(
    uint64_t seed,
    const std::vector<PreparedJavaStructureValidationConstraint> &constraints,
    const std::string &version_token,
    const CompiledQueryPlanContext &query_plan,
    LootValidationServerClient &loot_client,
    std::vector<NativeQueryMatchOut> &match_buffer,
    std::unordered_set<std::string> &skip_cache
) {
    if (constraints.empty()) {
        return true;
    }
    (void)skip_cache;

    uint32_t match_count = 0U;
    uint32_t biome_count = 0U;
    uint32_t cap_pruned = 0U;
    auto collect_details = [&](uint32_t cap) -> int {
        return query_plan.collect_details(
            seed,
            cap == 0U ? nullptr : match_buffer.data(),
            cap,
            &match_count,
            nullptr,
            0U,
            &biome_count,
            &cap_pruned,
            false
        );
    };

    int detail_rc = collect_details(static_cast<uint32_t>(match_buffer.size()));
    if (detail_rc < 0) {
        throw std::runtime_error("scanner_core_collect_query_details failed with code " + fmt_i64(detail_rc));
    }
    if (detail_rc != 0) {
        return false;
    }
    if (match_count > match_buffer.size()) {
        match_buffer.resize(match_count);
        detail_rc = collect_details(static_cast<uint32_t>(match_buffer.size()));
        if (detail_rc < 0) {
            throw std::runtime_error("scanner_core_collect_query_details failed with code " + fmt_i64(detail_rc));
        }
        if (detail_rc != 0) {
            return false;
        }
    }

    for (const PreparedJavaStructureValidationConstraint &constraint : constraints) {
        uint32_t valid_matches = 0U;
        std::unordered_set<uint64_t> seen_positions;
        seen_positions.reserve(32U);
        for (uint32_t i = 0; i < match_count; ++i) {
            const NativeQueryMatchOut &match = match_buffer[i];
            if (match.constraint_index != constraint.constraint_index) {
                continue;
            }
            const uint64_t packed = pack_match_position_key(match.x, match.z);
            if (!seen_positions.insert(packed).second) {
                continue;
            }
            StructureValidationOutcome outcome = StructureValidationOutcome::skipped;
            std::string validation_error;
            if (!loot_client.structure_valid(
                    constraint.structure,
                    version_token,
                    seed,
                    match.x,
                    match.z,
                    outcome,
                    validation_error)) {
                throw std::runtime_error(
                    "LootValidationServer STRUCTURE validation failed for constraint '" +
                    constraint.constraint_id + "' at (" + std::to_string(match.x) + "," +
                    std::to_string(match.z) + "): " + validation_error
                );
            }
            if (outcome == StructureValidationOutcome::skipped) {
                throw std::runtime_error(
                    "LootValidationServer STRUCTURE validation skipped for exact constraint '" +
                    constraint.constraint_id + "' at (" + std::to_string(match.x) + "," +
                    std::to_string(match.z) + "). Exact structure validation requires the Minecraft 26.1.2 "
                    "runtime/data to be available."
                );
            }
            if (outcome == StructureValidationOutcome::valid) {
                ++valid_matches;
            }
            if (valid_matches >= constraint.min_required) {
                break;
            }
        }
        if (valid_matches < constraint.min_required) {
            return false;
        }
    }
    return true;
}

static uint32_t validate_structure_settings_for_seed_batch(
    const uint64_t *seeds,
    uint32_t seed_count,
    const std::vector<PreparedStructureSettingsFilter> &filters,
    const std::string &version_token,
    const CompiledQueryPlanContext &query_plan,
    LootValidationServerClient &loot_client,
    std::vector<NativeQueryMatchOut> &match_buffer,
    std::vector<StructureSettingsRequest> &requests,
    std::vector<uint32_t> &request_seed_indices,
    std::vector<bool> &request_results,
    std::vector<uint64_t> &current,
    std::vector<uint64_t> &next,
    uint64_t *out
) {
    if (filters.empty() || seed_count == 0U) {
        if (out != seeds) {
            std::copy_n(seeds, seed_count, out);
        }
        return seed_count;
    }

    current.assign(seeds, seeds + seed_count);
    next.clear();
    for (const PreparedStructureSettingsFilter &filter : filters) {
        requests.clear();
        request_seed_indices.clear();
        std::vector<char> seed_passed(current.size(), 0);

        for (size_t seed_index = 0; seed_index < current.size(); ++seed_index) {
            const uint64_t seed = current[seed_index];
            uint32_t match_count = 0U;
            uint32_t biome_count = 0U;
            uint32_t cap_pruned = 0U;
            auto collect_details = [&](uint32_t cap) -> int {
                return query_plan.collect_details(
                    seed,
                    cap == 0U ? nullptr : match_buffer.data(),
                    cap,
                    &match_count,
                    nullptr,
                    0U,
                    &biome_count,
                    &cap_pruned,
                    false
                );
            };

            int detail_rc = collect_details(static_cast<uint32_t>(match_buffer.size()));
            if (detail_rc < 0) {
                throw std::runtime_error("scanner_core_collect_query_details failed with code " + fmt_i64(detail_rc));
            }
            if (detail_rc != 0) {
                continue;
            }
            if (match_count > match_buffer.size()) {
                match_buffer.resize(match_count);
                detail_rc = collect_details(static_cast<uint32_t>(match_buffer.size()));
                if (detail_rc < 0) {
                    throw std::runtime_error("scanner_core_collect_query_details failed with code " + fmt_i64(detail_rc));
                }
                if (detail_rc != 0) {
                    continue;
                }
            }

            std::unordered_set<uint64_t> seen_positions;
            seen_positions.reserve(16U);
            for (uint32_t i = 0; i < match_count; ++i) {
                const NativeQueryMatchOut &match = match_buffer[i];
                if (match.constraint_index != filter.constraint_index) {
                    continue;
                }
                const uint64_t packed = pack_match_position_key(match.x, match.z);
                if (!seen_positions.insert(packed).second) {
                    continue;
                }
                StructureSettingsRequest req{};
                req.structure = filter.structure;
                req.seed = seed;
                req.block_x = match.x;
                req.block_z = match.z;
                req.encoded_settings = filter.encoded_settings;
                requests.push_back(std::move(req));
                request_seed_indices.push_back(static_cast<uint32_t>(seed_index));
            }
        }

        if (!requests.empty()) {
            std::string error;
            if (!loot_client.settings_many(version_token, requests, request_results, error)) {
                throw std::runtime_error(
                    "LootValidationServer SETTINGS_BATCH failed for constraint '" + filter.constraint_id + "': " + error
                );
            }
            for (size_t i = 0; i < request_results.size(); ++i) {
                if (request_results[i]) {
                    seed_passed[request_seed_indices[i]] = 1;
                }
            }
        }

        next.clear();
        next.reserve(current.size());
        for (size_t i = 0; i < current.size(); ++i) {
            if (seed_passed[i]) {
                next.push_back(current[i]);
            }
        }
        current.swap(next);
        if (current.empty()) {
            break;
        }
    }

    if (!current.empty()) {
        std::copy(current.begin(), current.end(), out);
    }
    return static_cast<uint32_t>(current.size());
}

static bool validate_loot_filters_for_seed(
    uint64_t seed,
    const std::vector<PreparedLootFilter> &loot_filters,
    const std::string &version_token,
    const CompiledQueryPlanContext &query_plan,
    LootValidationServerClient &loot_client,
    std::vector<NativeQueryMatchOut> &match_buffer,
    std::unordered_map<std::string, std::unordered_map<std::string, int>> &roll_cache
) {
    if (loot_filters.empty()) {
        return true;
    }
    (void)roll_cache;

    uint32_t match_count = 0U;
    uint32_t biome_count = 0U;
    uint32_t cap_pruned = 0U;
    auto collect_details = [&](uint32_t cap) -> int {
        return query_plan.collect_details(
            seed,
            cap == 0U ? nullptr : match_buffer.data(),
            cap,
            &match_count,
            nullptr,
            0U,
            &biome_count,
            &cap_pruned,
            false
        );
    };

    int detail_rc = collect_details(static_cast<uint32_t>(match_buffer.size()));
    if (detail_rc < 0) {
        throw std::runtime_error("scanner_core_collect_query_details failed with code " + fmt_i64(detail_rc));
    }
    if (detail_rc != 0) {
        return false;
    }
    if (match_count > match_buffer.size()) {
        match_buffer.resize(match_count);
        detail_rc = collect_details(static_cast<uint32_t>(match_buffer.size()));
        if (detail_rc < 0) {
            throw std::runtime_error("scanner_core_collect_query_details failed with code " + fmt_i64(detail_rc));
        }
        if (detail_rc != 0) {
            return false;
        }
    }

    roll_cache.clear();
    std::unordered_map<uint64_t, bool> exact_shipwreck_variant_cache;
    exact_shipwreck_variant_cache.reserve(32U);
    for (const PreparedLootFilter &filter : loot_filters) {
        bool passed = false;
        std::unordered_set<uint64_t> seen_positions;
        seen_positions.reserve(32U);
        for (uint32_t i = 0; i < match_count; ++i) {
            const NativeQueryMatchOut &match = match_buffer[i];
            if (match.constraint_index != filter.constraint_index) {
                continue;
            }
            const uint64_t packed = pack_match_position_key(match.x, match.z);
            if (!seen_positions.insert(packed).second) {
                continue;
            }

            const std::string roll_structure = resolve_effective_loot_roll_structure(
                filter,
                version_token,
                seed,
                query_plan.cubi_mc_version,
                match.x,
                match.z,
                exact_shipwreck_variant_cache
            );

            bool roll_passed = false;
            std::string roll_error;
            if (!loot_client.check(
                    roll_structure,
                    version_token,
                    seed,
                    match.x,
                    match.z,
                    filter.encoded_required,
                    roll_passed,
                    roll_error)) {
                throw std::runtime_error(
                    "LootValidationServer failed for constraint '" + filter.constraint_id + "' at (" +
                    std::to_string(match.x) + "," + std::to_string(match.z) + "): " + roll_error
                );
            }

            if (roll_passed) {
                passed = true;
                break;
            }
        }
        if (!passed) {
            return false;
        }
    }
    return true;
}

static uint32_t validate_loot_filters_for_seed_batch(
    const uint64_t *seeds,
    uint32_t seed_count,
    const std::vector<PreparedLootFilter> &loot_filters,
    const std::string &version_token,
    const CompiledQueryPlanContext &query_plan,
    LootValidationServerClient &loot_client,
    std::vector<NativeQueryMatchOut> &match_buffer,
    std::vector<LootCheckRequest> &check_requests,
    std::vector<uint32_t> &check_seed_indices,
    std::vector<bool> &check_results,
    std::vector<uint64_t> &current,
    std::vector<uint64_t> &next,
    uint64_t *out_valid
) {
    if (seed_count == 0U) {
        return 0U;
    }
    if (loot_filters.empty()) {
        if (out_valid != seeds) {
            std::copy_n(seeds, seed_count, out_valid);
        }
        return seed_count;
    }

    current.assign(seeds, seeds + seed_count);
    next.clear();
    for (const PreparedLootFilter &filter : loot_filters) {
        check_requests.clear();
        check_seed_indices.clear();
        std::vector<char> seed_passed(current.size(), 0);

        for (size_t seed_index = 0; seed_index < current.size(); ++seed_index) {
            const uint64_t seed = current[seed_index];
            uint32_t match_count = 0U;
            uint32_t biome_count = 0U;
            uint32_t cap_pruned = 0U;
            if (!try_collect_fast_loot_matches_for_filter(seed, filter, query_plan, match_buffer, match_count)) {
                auto collect_details = [&](uint32_t cap) -> int {
                    return query_plan.collect_details(
                        seed,
                        cap == 0U ? nullptr : match_buffer.data(),
                        cap,
                        &match_count,
                        nullptr,
                        0U,
                        &biome_count,
                        &cap_pruned,
                        false
                    );
                };

                int detail_rc = collect_details(static_cast<uint32_t>(match_buffer.size()));
                if (detail_rc < 0) {
                    throw std::runtime_error("scanner_core_collect_query_details failed with code " + fmt_i64(detail_rc));
                }
                if (detail_rc != 0) {
                    continue;
                }
                if (match_count > match_buffer.size()) {
                    match_buffer.resize(match_count);
                    detail_rc = collect_details(static_cast<uint32_t>(match_buffer.size()));
                    if (detail_rc < 0) {
                        throw std::runtime_error(
                            "scanner_core_collect_query_details failed with code " + fmt_i64(detail_rc));
                    }
                    if (detail_rc != 0) {
                        continue;
                    }
                }
            }

            std::unordered_map<uint64_t, bool> exact_shipwreck_variant_cache;
            exact_shipwreck_variant_cache.reserve(8U);
            std::unordered_set<uint64_t> seen_positions;
            seen_positions.reserve(32U);
            for (uint32_t i = 0; i < match_count; ++i) {
                const NativeQueryMatchOut &match = match_buffer[i];
                if (match.constraint_index != filter.constraint_index) {
                    continue;
                }
                const uint64_t packed = pack_match_position_key(match.x, match.z);
                if (!seen_positions.insert(packed).second) {
                    continue;
                }
                check_requests.push_back(LootCheckRequest{
                    resolve_effective_loot_roll_structure(
                        filter,
                        version_token,
                        seed,
                        query_plan.cubi_mc_version,
                        match.x,
                        match.z,
                        exact_shipwreck_variant_cache),
                    seed,
                    match.x,
                    match.z,
                    filter.encoded_required,
                });
                check_seed_indices.push_back(static_cast<uint32_t>(seed_index));
            }
        }

        if (!check_requests.empty()) {
            std::string roll_error;
            if (!loot_client.check_many(version_token, check_requests, check_results, roll_error)) {
                throw std::runtime_error(
                    "LootValidationServer failed during batched loot validation: " + roll_error);
            }
            for (size_t i = 0; i < check_results.size(); ++i) {
                if (check_results[i]) {
                    seed_passed[check_seed_indices[i]] = 1;
                }
            }
        }

        next.clear();
        next.reserve(current.size());
        for (size_t i = 0; i < current.size(); ++i) {
            if (seed_passed[i] != 0) {
                next.push_back(current[i]);
            }
        }
        current.swap(next);
        if (current.empty()) {
            break;
        }
    }

    if (!current.empty()) {
        std::copy(current.begin(), current.end(), out_valid);
    }
    return static_cast<uint32_t>(current.size());
}

static std::string summarize_loot_requirements(const std::vector<PreparedLootRequirement> &required) {
    if (required.empty()) {
        return "_";
    }
    std::ostringstream oss;
    for (size_t i = 0; i < required.size(); ++i) {
        if (i > 0U) {
            oss << "; ";
        }
        oss << required[i].item << ':' << required[i].count;
    }
    return oss.str();
}

static void print_seed_detail_report(
    uint64_t seed,
    const CliArgs &args,
    const QueryRuntime &runtime,
    const CompiledQueryPlanContext &query_plan,
    const std::vector<PreparedLootFilter> &prepared_loot_filters,
    const std::string &loot_version_token,
    LootValidationServerClient *loot_client
) {
    const std::vector<NativeQueryMatchOut> matches = collect_query_match_details_for_seed(seed, query_plan);

    std::cout << "DETAIL seed raw=" << fmt_seed(seed) << " lift64=" << fmt_seed(lift_seed_to_64(seed, args.upper16))
              << "\n";
    if (matches.empty()) {
        std::cout << "DETAIL no structure match details available.\n";
        return;
    }

    for (uint32_t constraint_index = 0U; constraint_index < runtime.ordered_constraint_ids.size(); ++constraint_index) {
        const std::string &cid = runtime.ordered_constraint_ids[constraint_index];
        const QueryConstraintRuntime &rc = runtime.constraints.at(cid);
        bool printed_constraint_header = false;
        std::unordered_set<uint64_t> seen_positions;
        seen_positions.reserve(16U);
        for (const NativeQueryMatchOut &match : matches) {
            if (match.constraint_index != constraint_index) {
                continue;
            }
            const uint64_t packed = pack_match_position_key(match.x, match.z);
            if (!seen_positions.insert(packed).second) {
                continue;
            }
            if (!printed_constraint_header) {
                std::cout << "MATCH constraint=" << cid << " structure=" << rc.spec.structure
                          << " mode=" << rc.spec.mode << " anchor=" << rc.spec.anchor << "\n";
                printed_constraint_header = true;
            }
            std::cout << "  -> x=" << match.x << " z=" << match.z
                      << " anchor_x=" << match.anchor_x << " anchor_z=" << match.anchor_z
                      << " dist=" << fmt_double(std::sqrt(static_cast<double>(match.dist2)), 1) << " blocks\n";
        }
    }

    if (prepared_loot_filters.empty() || loot_client == nullptr) {
        return;
    }

    std::unordered_map<uint64_t, bool> exact_shipwreck_variant_cache;
    exact_shipwreck_variant_cache.reserve(32U);
    for (const PreparedLootFilter &filter : prepared_loot_filters) {
        std::unordered_set<uint64_t> seen_positions;
        seen_positions.reserve(16U);
        bool printed = false;
        for (const NativeQueryMatchOut &match : matches) {
            if (match.constraint_index != filter.constraint_index) {
                continue;
            }
            const uint64_t packed = pack_match_position_key(match.x, match.z);
            if (!seen_positions.insert(packed).second) {
                continue;
            }

            const std::string roll_structure = resolve_effective_loot_roll_structure(
                filter,
                loot_version_token,
                seed,
                query_plan.cubi_mc_version,
                match.x,
                match.z,
                exact_shipwreck_variant_cache
            );

            LootRollDetail detail{};
            std::string error;
            if (!loot_client->roll_detail(
                    roll_structure,
                    loot_version_token,
                    seed,
                    match.x,
                    match.z,
                    detail,
                    error)) {
                throw std::runtime_error(
                    "LootValidationServer detail lookup failed for constraint '" + filter.constraint_id + "' at (" +
                    std::to_string(match.x) + "," + std::to_string(match.z) + "): " + error
                );
            }

            if (is_exact_loot_version_token(loot_version_token) &&
                is_exact_village_candidate_loot_structure_id(roll_structure)) {
                const LootChestDetail *matched_chest = nullptr;
                for (const LootChestDetail &chest : detail.chests) {
                    if (loot_counts_satisfy_requirements(chest.counts, filter.required)) {
                        matched_chest = &chest;
                        break;
                    }
                }
                if (matched_chest == nullptr) {
                    continue;
                }

                std::cout << "LOOT constraint=" << filter.constraint_id << " structure=" << roll_structure
                          << " requirements=" << summarize_loot_requirements(filter.required)
                          << " counts=" << format_loot_count_map(matched_chest->counts) << "\n";
                std::cout << "  CHEST table=" << matched_chest->table_id << " pos=(" << matched_chest->x << ',';
                if (matched_chest->y < 0) {
                    std::cout << '?';
                } else {
                    std::cout << matched_chest->y;
                }
                std::cout << ',' << matched_chest->z << ") counts="
                          << format_loot_count_map(matched_chest->counts) << "\n";
                printed = true;
                break;
            }

            if (!loot_counts_satisfy_requirements(detail.aggregate_counts, filter.required)) {
                continue;
            }

            std::cout << "LOOT constraint=" << filter.constraint_id << " structure=" << roll_structure
                      << " requirements=" << summarize_loot_requirements(filter.required)
                      << " counts=" << format_loot_count_map(detail.aggregate_counts) << "\n";
            for (const LootChestDetail &chest : detail.chests) {
                std::cout << "  CHEST table=" << chest.table_id << " pos=(" << chest.x << ',';
                if (chest.y < 0) {
                    std::cout << '?';
                } else {
                    std::cout << chest.y;
                }
                std::cout << ',' << chest.z << ") counts=" << format_loot_count_map(chest.counts) << "\n";
            }
            printed = true;
            break;
        }
        if (!printed) {
            std::cout << "LOOT constraint=" << filter.constraint_id << " requirements="
                      << summarize_loot_requirements(filter.required)
                      << " matched=no satisfying chest set found in detailed replay\n";
        }
    }
}

static uint64_t lift_seed_to_64(uint64_t seed, uint16_t upper16) {
    return (static_cast<uint64_t>(upper16) << 48U) | (seed & JAVA_MASK);
}

static void emit_seed_found_line(uint64_t seed, const CliArgs &args, const char *stage = nullptr) {
    const uint64_t raw_seed = seed & JAVA_MASK;
    const uint64_t lifted = lift_seed_to_64(raw_seed, args.upper16);
    std::cout << "SEED_FOUND ";
    if (stage != nullptr && stage[0] != '\0') {
        std::cout << "stage=" << stage << " ";
    }
    if (args.output_mode == "raw") {
        std::cout << "raw=" << fmt_seed(raw_seed) << "\n";
    } else if (args.output_mode == "lift64") {
        std::cout << "lift64=" << fmt_seed(lifted) << "\n";
    } else {
        std::cout << "raw=" << fmt_seed(raw_seed) << " lift64=" << fmt_seed(lifted) << "\n";
    }
}

static void write_seed_txt_line(std::ostream &out, uint64_t seed, const CliArgs &args) {
    const uint64_t raw_seed = seed & JAVA_MASK;
    const uint64_t lifted = lift_seed_to_64(raw_seed, args.upper16);
    if (args.output_mode == "lift64") {
        out << fmt_seed(lifted) << "\n";
    } else if (args.output_mode == "both") {
        out << "raw=" << fmt_seed(raw_seed) << " lift64=" << fmt_seed(lifted) << "\n";
    } else {
        out << fmt_seed(raw_seed) << "\n";
    }
}

static std::string read_text_file_utf8_bom_tolerant(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open query file: " + path.string());
    }
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(out.begin(), out.begin() + 3);
    }
    return out;
}

static void print_usage() {
    std::cout << native_scanner_program_name() << " --query-json <json> [options]\n";
    std::cout << "Anchors: origin | spawn | constraint:<id> | fixed:x,z\n";
    std::cout
        << "Biome shape helper: biome_filters[].shape={\"type\":\"circle|square|diamond\",\"center\":[x,z],"
        << "\"radius\":R,\"edge\":\"outward|inward\"}\n";
    std::cout << "Options:\n";
    std::cout << "  --query-file <path>\n";
    std::cout << "  --start <seed>\n";
    std::cout << "  --count <n>\n";
    std::cout << "  --batch-size <n|balanced|max>\n";
    std::cout << "  --execution-mode <auto|scalar|simd-x4|simd-x8|max-throughput>\n";
    std::cout << "  --max-print <n>\n";
    std::cout << "  --mc-version <ver>\n";
    std::cout << "  --strict-workers <n>\n";
    std::cout << "  --output-mode <raw|lift64|both>\n";
    std::cout << "  --upper16 <n>\n";
    std::cout << "  --print-closest\n";
    std::cout << "  --progress-interval <seconds>\n";
    std::cout << "  --control-stdin\n";
    std::cout << "  --terminal-mode\n";
    std::cout << "  --stream-seeds\n";
    std::cout << "  --debug\n";
    std::cout << "  --plan-diagnostics\n";
    std::cout << "  --shadow-compare\n";
    std::cout << "  --completeness <exact|approximate> (default exact; approximate enables lossy speed shortcuts)\n";
    std::cout << "  --scan-mode <placement|strict|mixed> (default strict)\n";
    std::cout << "  --mixed-queue-capacity <n>          (default 1000000 survivors)\n";
    std::cout << "  --mixed-survivor-cap <n>            (0 disables per-batch survivor cap)\n";
    std::cout << "  --mixed-adaptive-throttling <on|off>\n";
    std::cout << "  --save-txt <path>                   (write verified/output seeds)\n";
    std::cout << "  --gpu-pipeline <auto|async|sync>   (auto: pick async when the loaded gpu_filter supports it)\n";
    std::cout << "  --gpu-backend <auto|cuda|opencl>   (auto: try CUDA first, fall back to OpenCL)\n";
    std::cout << "  --gpu-inflight-batches <auto|1..8> (mixed async GPU queue depth; default auto)\n";
    std::cout << "  --compiled-stage-a-mode <auto|single|multi|cpu-a5|gpu-off> (default auto)\n";
    std::cout << "  --java-cubiomes-validation <auto|off|strict> (default auto)\n";
    std::cout << "  --list-structures\n";
    std::cout << "  --dump-query-template\n";
}

static uint64_t parse_u64(const std::string &raw) {
    size_t idx = 0;
    const unsigned long long v = std::stoull(raw, &idx, 0);
    if (idx != raw.size()) {
        throw std::runtime_error("Invalid integer: " + raw);
    }
    return static_cast<uint64_t>(v);
}

static int parse_i32(const std::string &raw) {
    size_t idx = 0;
    const long long v = std::stoll(raw, &idx, 0);
    if (idx != raw.size() || v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
        throw std::runtime_error("Invalid integer: " + raw);
    }
    return static_cast<int>(v);
}

static CliArgs parse_cli(int argc, char **argv) {
    CliArgs args{};
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        auto require_value = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + name);
            }
            ++i;
            return argv[i];
        };

        if (opt == "--help" || opt == "-h") {
            print_usage();
            std::exit(0);
        } else if (opt == "--query-json") {
            args.query_json = require_value("--query-json");
        } else if (opt == "--query-file") {
            args.query_file = require_value("--query-file");
        } else if (opt == "--start") {
            args.start = parse_u64(require_value("--start"));
            args.has_start = true;
        } else if (opt == "--count") {
            args.count = parse_u64(require_value("--count"));
        } else if (opt == "--batch-size") {
            args.batch_size = lower_ascii(trim(require_value("--batch-size")));
        } else if (opt == "--execution-mode") {
            args.execution_mode = normalize_execution_mode(require_value("--execution-mode"));
            if (args.execution_mode.empty()) {
                throw std::runtime_error(
                    "Invalid --execution-mode. Use auto|scalar|simd-x4|simd-x8|max-throughput."
                );
            }
        } else if (opt == "--max-print") {
            args.max_print = std::max(1, parse_i32(require_value("--max-print")));
        } else if (opt == "--mc-version") {
            args.mc_version = require_value("--mc-version");
        } else if (opt == "--strict-workers") {
            args.strict_workers = std::max(0, parse_i32(require_value("--strict-workers")));
            args.strict_workers_set = true;
        } else if (opt == "--output-mode") {
            args.output_mode = lower_ascii(trim(require_value("--output-mode")));
            if (args.output_mode != "raw" && args.output_mode != "lift64" && args.output_mode != "both") {
                throw std::runtime_error("Invalid --output-mode.");
            }
        } else if (opt == "--upper16") {
            args.upper16 = static_cast<uint16_t>(parse_u64(require_value("--upper16")) & 0xFFFFULL);
        } else if (opt == "--progress-interval") {
            args.progress_interval = std::stod(require_value("--progress-interval"));
            if (args.progress_interval < 0.0) {
                args.progress_interval = 0.0;
            }
        } else if (opt == "--print-closest") {
            args.print_closest = true;
        } else if (opt == "--list-structures") {
            args.list_structures = true;
        } else if (opt == "--dump-query-template") {
            args.dump_query_template = true;
        } else if (opt == "--control-stdin") {
            args.control_stdin = true;
        } else if (opt == "--terminal-mode") {
            args.terminal_mode = true;
        } else if (opt == "--stream-seeds") {
            args.stream_seeds = true;
        } else if (opt == "--debug") {
            args.debug_enabled = true;
        } else if (opt == "--plan-diagnostics") {
            args.plan_diagnostics = true;
        } else if (opt == "--shadow-compare") {
            args.shadow_compare = true;
        } else if (opt == "--completeness" || opt == "--safety-completeness") {
            args.completeness_policy = normalize_completeness_policy(require_value(opt.c_str()));
            if (args.completeness_policy.empty()) {
                throw std::runtime_error("Invalid --completeness. Use exact|approximate.");
            }
        } else if (opt == "--scan-mode") {
            const std::string val = lower_ascii(trim(require_value("--scan-mode")));
            if (val != "placement" && val != "strict" && val != "mixed") {
                throw std::runtime_error("Invalid --scan-mode. Use placement|strict|mixed.");
            }
            args.scan_mode = val;
        } else if (opt == "--mixed-queue-capacity") {
            args.mixed_queue_capacity = std::max<uint64_t>(1ULL, parse_u64(require_value("--mixed-queue-capacity")));
        } else if (opt == "--mixed-survivor-cap") {
            args.mixed_survivor_cap = static_cast<uint32_t>(
                std::min<uint64_t>(parse_u64(require_value("--mixed-survivor-cap")), std::numeric_limits<uint32_t>::max()));
        } else if (opt == "--mixed-adaptive-throttling") {
            const std::string val = lower_ascii(trim(require_value("--mixed-adaptive-throttling")));
            if (val == "on" || val == "true" || val == "yes" || val == "1") {
                args.mixed_adaptive_throttling = true;
            } else if (val == "off" || val == "false" || val == "no" || val == "0") {
                args.mixed_adaptive_throttling = false;
            } else {
                throw std::runtime_error("Invalid --mixed-adaptive-throttling. Use on|off.");
            }
        } else if (opt == "--save-txt") {
            args.save_txt = require_value("--save-txt");
        } else if (opt == "--gpu-pipeline") {
            const std::string val = lower_ascii(trim(require_value("--gpu-pipeline")));
            if (val != "auto" && val != "async" && val != "sync") {
                throw std::runtime_error("Invalid --gpu-pipeline. Use auto|async|sync.");
            }
            args.gpu_pipeline = val;
        } else if (opt == "--gpu-backend") {
            const std::string val = lower_ascii(trim(require_value("--gpu-backend")));
            if (val != "auto" && val != "cuda" && val != "opencl") {
                throw std::runtime_error("Invalid --gpu-backend. Use auto|cuda|opencl.");
            }
            args.gpu_backend = val;
        } else if (opt == "--gpu-inflight-batches") {
            const std::string val = lower_ascii(trim(require_value("--gpu-inflight-batches")));
            if (val != "auto") {
                const uint64_t parsed = parse_u64(val);
                if (parsed < 1ULL || parsed > 8ULL) {
                    throw std::runtime_error("Invalid --gpu-inflight-batches. Use auto or 1..8.");
                }
            }
            args.gpu_inflight_batches = val;
        } else if (opt == "--compiled-stage-a-mode") {
            const std::string normalized = normalize_compiled_stage_a_mode(require_value("--compiled-stage-a-mode"));
            if (normalized.empty()) {
                throw std::runtime_error(
                    "Invalid --compiled-stage-a-mode. Use auto|single|multi|cpu-a5|gpu-off."
                );
            }
            args.compiled_stage_a_mode = normalized;
            args.compiled_stage_a_mode_set = true;
        } else if (opt == "--java-cubiomes-validation") {
            args.java_cubiomes_validation = lower_ascii(trim(require_value("--java-cubiomes-validation")));
            if (args.java_cubiomes_validation != "auto" && args.java_cubiomes_validation != "off" &&
                args.java_cubiomes_validation != "strict") {
                throw std::runtime_error("Invalid --java-cubiomes-validation. Use auto|off|strict.");
            }
        } else {
            throw std::runtime_error("Unknown argument: " + opt);
        }
    }
    return args;
}

static fs::path detect_repo_root(const char *argv0) {
    fs::path exe = fs::absolute(fs::path(argv0));
    fs::path dir = exe.parent_path();
    const std::string folder = lower_ascii(dir.filename().string());
    if (folder == "native" || folder == "macos") {
        return dir.parent_path();
    }
    return dir;
}

static fs::path resolve_cubiomes_lib_path(const fs::path &repo_root) {
    const char *env = std::getenv("SCANNER_CUBIOMES_DLL");
    if (env != nullptr && *env != '\0') {
        fs::path p(env);
        if (fs::exists(p)) {
            return p;
        }
    }
    const std::vector<fs::path> candidates = {
        repo_root / "cubiomes_26.1.2_fork" / "lib" / cubiomes_library_filename(),
#if !defined(_WIN32)
#if defined(__APPLE__)
        repo_root / "cubiomes_26.1.2_fork" / "build_macos" / "libcubiomes.dylib",
#endif
        repo_root / "cubiomes_26.1.2_fork" / "build" / "libcubiomes.so",
#endif
        repo_root / "cubiomespi" / "lib" / cubiomes_library_filename(),
        repo_root / cubiomes_library_filename(),
        repo_root / "venv" / "Lib" / "site-packages" / "cubiomespi" / "lib" / cubiomes_library_filename(),
    };
    for (const fs::path &candidate : candidates) {
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
#if !defined(_WIN32)
    const fs::path venv_lib = repo_root / "venv" / "lib";
    if (fs::exists(venv_lib)) {
        for (const auto &entry : fs::directory_iterator(venv_lib, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_directory()) {
                continue;
            }
            const fs::path candidate = entry.path() / "site-packages" / "cubiomespi" / "lib" / cubiomes_library_filename();
            if (fs::exists(candidate)) {
                return candidate;
            }
        }
    }
#endif
    return {};
}

static uint64_t choose_default_start() {
    std::random_device rd;
    std::mt19937_64 gen(
        (static_cast<uint64_t>(rd()) << 32U) ^
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())
    );
    std::uniform_int_distribution<uint64_t> dist(0ULL, JAVA_MASK);
    return dist(gen) & JAVA_MASK;
}

static uint64_t recommended_batch_size(
    uint64_t requested,
    uint64_t total_count,
    bool strict_or_biome,
    uint64_t total_mem_bytes
) {
    const uint64_t req = std::max<uint64_t>(1, requested);
    const uint64_t cnt = std::max<uint64_t>(1, total_count);

    uint64_t target = strict_or_biome ? 250'000ULL : 500'000ULL;
    if (total_mem_bytes > 0) {
        const double gib = static_cast<double>(total_mem_bytes) / static_cast<double>(1024ULL * 1024ULL * 1024ULL);
        if (gib >= 20.0) {
            target = strict_or_biome ? 750'000ULL : 1'000'000ULL;
        } else if (gib >= 12.0) {
            target = strict_or_biome ? 500'000ULL : 750'000ULL;
        } else if (gib >= 8.0) {
            target = strict_or_biome ? 300'000ULL : 500'000ULL;
        } else {
            target = strict_or_biome ? 150'000ULL : 250'000ULL;
        }
    }

    if (cnt < 100'000ULL) {
        target = std::min<uint64_t>(target, 100'000ULL);
    } else if (cnt < 500'000ULL) {
        target = std::min<uint64_t>(target, 250'000ULL);
    }

    const uint64_t tuned = std::min<uint64_t>(cnt, target);
    return req >= tuned ? req : tuned;
}

static uint64_t estimate_max_batch_cap(bool strict_or_biome, uint64_t total_mem_bytes) {
    double gib = 8.0;
    if (total_mem_bytes > 0) {
        gib = static_cast<double>(total_mem_bytes) / static_cast<double>(1024ULL * 1024ULL * 1024ULL);
    }
    uint64_t cap = 0;
    if (gib >= 24.0) {
        cap = 8'000'000ULL;
    } else if (gib >= 16.0) {
        cap = 6'000'000ULL;
    } else if (gib >= 12.0) {
        cap = 4'000'000ULL;
    } else if (gib >= 8.0) {
        cap = 3'000'000ULL;
    } else {
        cap = 1'500'000ULL;
    }
    if (strict_or_biome) {
        cap = static_cast<uint64_t>(static_cast<double>(cap) * 0.7);
    }
    return std::max<uint64_t>(150'000ULL, cap);
}

static uint64_t estimate_max_probe_batch_cap(bool strict_or_biome, uint64_t total_mem_bytes) {
    double gib = 8.0;
    if (total_mem_bytes > 0) {
        gib = static_cast<double>(total_mem_bytes) / static_cast<double>(1024ULL * 1024ULL * 1024ULL);
    }

    uint64_t cap = 0;
    if (gib >= 24.0) {
        cap = 1'000'000'000ULL;
    } else if (gib >= 16.0) {
        cap = 768'000'000ULL;
    } else if (gib >= 12.0) {
        cap = 512'000'000ULL;
    } else if (gib >= 8.0) {
        cap = 384'000'000ULL;
    } else {
        cap = 192'000'000ULL;
    }
    if (strict_or_biome) {
        cap = static_cast<uint64_t>(static_cast<double>(cap) * 0.55);
    }
    return std::max<uint64_t>(estimate_max_batch_cap(strict_or_biome, total_mem_bytes), cap);
}

struct NvidiaSmiSample {
    bool ok = false;
    double gpu_util_pct = 0.0;
    double memory_used_pct = 0.0;
};

static NvidiaSmiSample read_nvidia_smi_sample() {
    NvidiaSmiSample sample{};
#if defined(_WIN32)
    FILE *pipe = _popen(
        "nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>nul",
        "r"
    );
#else
    FILE *pipe = popen(
        "nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null",
        "r"
    );
#endif
    if (pipe == nullptr) {
        return sample;
    }
    char buffer[256] = {};
    std::string line;
    if (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line = trim(buffer);
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    if (line.empty()) {
        return sample;
    }
    for (char &ch : line) {
        if (ch == ',') {
            ch = ' ';
        }
    }
    std::istringstream iss(line);
    double util = 0.0;
    double mem_used = 0.0;
    double mem_total = 0.0;
    if (!(iss >> util >> mem_used >> mem_total)) {
        return sample;
    }
    sample.ok = true;
    sample.gpu_util_pct = std::max(0.0, util);
    sample.memory_used_pct = mem_total > 0.0
        ? std::max(0.0, std::min(100.0, (mem_used / mem_total) * 100.0))
        : 0.0;
    return sample;
}

class NvidiaSmiSampler {
public:
    explicit NvidiaSmiSampler(bool enabled) : enabled_(enabled) {
        if (!enabled_) {
            return;
        }
        NvidiaSmiSample initial = read_nvidia_smi_sample();
        if (!initial.ok) {
            enabled_ = false;
            return;
        }
        record(initial);
        running_.store(true);
        worker_ = std::thread([this]() {
            while (running_.load()) {
                NvidiaSmiSample sample = read_nvidia_smi_sample();
                if (sample.ok) {
                    record(sample);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        });
    }

    ~NvidiaSmiSampler() {
        stop();
    }

    void stop() {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool available() const {
        return enabled_ && sample_count_.load() > 0U;
    }

    double max_gpu_util_pct() const {
        return max_gpu_util_pct_.load();
    }

    double max_memory_used_pct() const {
        return max_memory_used_pct_.load();
    }

private:
    void record(const NvidiaSmiSample &sample) {
        sample_count_.fetch_add(1U);
        double old_util = max_gpu_util_pct_.load();
        while (sample.gpu_util_pct > old_util &&
               !max_gpu_util_pct_.compare_exchange_weak(old_util, sample.gpu_util_pct)) {
        }
        double old_mem = max_memory_used_pct_.load();
        while (sample.memory_used_pct > old_mem &&
               !max_memory_used_pct_.compare_exchange_weak(old_mem, sample.memory_used_pct)) {
        }
    }

    bool enabled_ = false;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> sample_count_{0U};
    std::atomic<double> max_gpu_util_pct_{0.0};
    std::atomic<double> max_memory_used_pct_{0.0};
    std::thread worker_;
};

struct BatchProbeResult {
    double rate_sps = 0.0;
    bool has_gpu_sample = false;
    double gpu_util_pct = 0.0;
    double gpu_memory_used_pct = 0.0;
};

struct MixedAsyncTuningChoice {
    bool valid = false;
    uint64_t batch_size = 0ULL;
    uint32_t depth = 0U;
    double rate_sps = 0.0;
};

struct MixedAsyncProbeRecord {
    uint64_t batch_size = 0ULL;
    uint32_t depth = 0U;
    BatchProbeResult result{};
};

static bool is_gpu_probe_resource_failure(const std::string &msg) {
    return msg.find("out of memory") != std::string::npos ||
           msg.find("cuda out of memory") != std::string::npos ||
           msg.find("cannot allocate memory") != std::string::npos ||
           msg.find("bad_alloc") != std::string::npos ||
           msg.find("failed with status -4") != std::string::npos ||
           msg.find("failed with status -5") != std::string::npos ||
           msg.find("failed with status -6") != std::string::npos ||
           msg.find("failed with status -7") != std::string::npos;
}

static std::optional<BatchProbeResult> benchmark_batch_candidate(
    const std::function<void(uint64_t)> &run_probe,
    uint64_t batch_size,
    bool sample_gpu_utilization
) {
    try {
        NvidiaSmiSampler sampler(sample_gpu_utilization);
        const auto t0 = std::chrono::high_resolution_clock::now();
        uint32_t repetitions = 0U;
        double dt = 0.0;
        const double min_sample_seconds = sample_gpu_utilization ? 2.25 : 0.0;
        const uint32_t max_repetitions = sample_gpu_utilization ? 96U : 1U;
        do {
            run_probe(batch_size);
            ++repetitions;
            dt = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
        } while (sampler.available() && dt < min_sample_seconds && repetitions < max_repetitions);
        sampler.stop();
        if (dt <= 0.0) {
            return std::nullopt;
        }
        BatchProbeResult result{};
        result.rate_sps = (static_cast<double>(batch_size) * static_cast<double>(repetitions)) / dt;
        result.has_gpu_sample = sampler.available();
        result.gpu_util_pct = sampler.max_gpu_util_pct();
        result.gpu_memory_used_pct = sampler.max_memory_used_pct();
        return result;
    } catch (const std::bad_alloc &) {
        return std::nullopt;
    } catch (const std::runtime_error &exc) {
        const std::string msg = lower_ascii(exc.what());
        if (is_gpu_probe_resource_failure(msg)) {
            return std::nullopt;
        }
        throw;
    }
}

static std::vector<uint64_t> build_max_batch_candidates(uint64_t base, uint64_t cap, bool include_extended_points) {
    std::vector<uint64_t> candidates;
    auto push_candidate = [&](uint64_t value) {
        if (value == 0ULL || cap == 0ULL) {
            return;
        }
        candidates.push_back(std::max<uint64_t>(1ULL, std::min<uint64_t>(value, cap)));
    };

    push_candidate(base);
    const uint64_t core_probe_points[] = {
        8'000'000ULL,
        16'000'000ULL,
        32'000'000ULL,
        64'000'000ULL,
        96'000'000ULL,
        128'000'000ULL,
        192'000'000ULL,
        256'000'000ULL,
    };
    for (uint64_t point : core_probe_points) {
        push_candidate(point);
    }
    if (include_extended_points) {
        const uint64_t extended_probe_points[] = {
            384'000'000ULL,
            512'000'000ULL,
            768'000'000ULL,
            1'000'000'000ULL,
        };
        for (uint64_t point : extended_probe_points) {
            push_candidate(point);
        }
    }

    uint64_t walk_bs = std::max<uint64_t>(1ULL, std::min<uint64_t>(base, cap));
    while (walk_bs < cap) {
        const uint64_t next_bs =
            std::min<uint64_t>(cap, static_cast<uint64_t>(static_cast<double>(walk_bs) * 1.5));
        if (next_bs <= walk_bs) {
            break;
        }
        push_candidate(next_bs);
        walk_bs = next_bs;
    }
    push_candidate(cap);

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

static uint64_t auto_find_batch_size(
    const std::string &mode,
    uint64_t total_count,
    bool strict_or_biome,
    uint64_t total_mem_bytes,
    const std::function<void(uint64_t)> &run_probe,
    bool verbose,
    uint32_t min_async_memory_slots = 1U
) {
    const uint64_t base = recommended_batch_size(1, total_count, strict_or_biome, total_mem_bytes);
    uint64_t cap = 0;
    if (mode == "max") {
        const uint64_t probe_cap = estimate_max_probe_batch_cap(strict_or_biome, total_mem_bytes);
        cap = std::min<uint64_t>(total_count, std::max<uint64_t>(base, probe_cap));
        if (min_async_memory_slots > 1U && total_mem_bytes > 0ULL) {
            const long double budget = static_cast<long double>(total_mem_bytes) * 0.72L;
            const long double bytes_per_seed =
                static_cast<long double>(min_async_memory_slots) * static_cast<long double>(sizeof(uint64_t));
            const uint64_t async_slot_cap = static_cast<uint64_t>(budget / bytes_per_seed);
            if (async_slot_cap > 0ULL) {
                cap = std::min<uint64_t>(cap, async_slot_cap);
            }
        }
    } else {
        cap = std::min<uint64_t>(total_count, estimate_max_batch_cap(strict_or_biome, total_mem_bytes));
    }
    if (cap == 0) {
        return std::max<uint64_t>(1, std::min<uint64_t>(base, total_count));
    }

    if (mode == "balanced") {
        const uint64_t selected = std::max<uint64_t>(1, std::min<uint64_t>(cap, total_count));
        if (verbose) {
            std::cout << "Auto batch finder (balanced): heuristic selected " << fmt_u64(selected)
                      << " (use --batch-size max to benchmark/probe)\n";
        }
        return selected;
    }

    if (mode == "max") {
        const int fail_limit = 3;
        uint64_t lo = std::max<uint64_t>(1, std::min<uint64_t>(base, cap));
        uint64_t hi_fail = 0;
        bool has_hi_fail = false;
        uint64_t largest_stable_bs = lo;
        uint64_t best_rate_bs = lo;
        double best_rate = -1.0;
        int tested = 0;
        int fail_count = 0;
        auto consider_probe = [&](uint64_t probe_bs, const BatchProbeResult &result) {
            if (result.rate_sps > best_rate) {
                best_rate = result.rate_sps;
                best_rate_bs = probe_bs;
            }
        };
        auto print_probe = [&](uint64_t probe_bs, const BatchProbeResult &result) {
            if (!verbose) {
                return;
            }
            std::cout << "- " << fmt_u64(probe_bs) << ": " << fmt_double(result.rate_sps, 0)
                      << " seeds/sec";
            if (result.has_gpu_sample) {
                std::cout << ", gpu=" << fmt_double(result.gpu_util_pct, 1)
                          << "%, vram=" << fmt_double(result.gpu_memory_used_pct, 1) << "%";
            } else {
                std::cout << ", gpu=nvidia-smi unavailable";
            }
            std::cout << "\n";
        };

        std::vector<uint64_t> candidates = build_max_batch_candidates(lo, cap, true);

        if (verbose) {
            std::cout << "Auto batch finder (max): probing " << fmt_size(candidates.size())
                      << " candidate(s) with safety guard (cap=" << fmt_u64(cap)
                      << ", fail_limit=" << fmt_i64(fail_limit);
            if (min_async_memory_slots > 1U) {
                std::cout << ", preserving_async_slots=" << fmt_u64(min_async_memory_slots);
            }
            std::cout << ")...\n";
        }

        auto probe_candidate = [&](uint64_t probe_bs) -> bool {
            const std::optional<BatchProbeResult> result = benchmark_batch_candidate(run_probe, probe_bs, true);
            if (!result.has_value()) {
                ++fail_count;
                if (verbose) {
                    std::cout << "- " << fmt_u64(probe_bs) << ": OOM/failed (" << fmt_i64(fail_count) << "/"
                              << fmt_i64(fail_limit) << ")\n";
                }
                hi_fail = probe_bs;
                has_hi_fail = true;
                if (fail_count >= fail_limit) {
                    if (verbose) {
                        std::cout << "Auto batch finder (max): fail limit reached; stopping probes.\n";
                    }
                    return false;
                }
                return false;
            }

            ++tested;
            print_probe(probe_bs, *result);
            largest_stable_bs = probe_bs;
            consider_probe(probe_bs, *result);
            return true;
        };

        for (uint64_t probe_bs : candidates) {
            if (has_hi_fail && probe_bs >= hi_fail) {
                break;
            }
            if (!probe_candidate(probe_bs)) {
                break;
            }
        }

        if (has_hi_fail && largest_stable_bs + 1U < hi_fail && fail_count < fail_limit) {
            uint64_t left = largest_stable_bs + 1U;
            uint64_t right = hi_fail - 1U;
            while (left <= right) {
                const uint64_t mid = left + ((right - left) / 2U);
                const std::optional<BatchProbeResult> result = benchmark_batch_candidate(run_probe, mid, true);
                if (!result.has_value()) {
                    ++fail_count;
                    if (mid == 0) {
                        break;
                    }
                    right = mid - 1U;
                    if (fail_count >= fail_limit) {
                        if (verbose) {
                            std::cout << "Auto batch finder (max): fail limit reached during refinement; stopping.\n";
                        }
                        break;
                    }
                } else {
                    ++tested;
                    print_probe(mid, *result);
                    largest_stable_bs = mid;
                    consider_probe(mid, *result);
                    left = mid + 1U;
                }
            }
        }

        if (tested == 0) {
            const uint64_t fallback = std::max<uint64_t>(1, std::min<uint64_t>(base, std::max<uint64_t>(1, cap)));
            if (verbose) {
                std::cout << "Auto batch finder (max): fallback to " << fmt_u64(fallback) << "\n";
            }
            return fallback;
        }
        uint64_t selected_bs = best_rate_bs;
        std::string selected_reason = "best throughput";
        if (verbose) {
            std::cout << "Auto batch finder (max): selected " << fmt_u64(selected_bs)
                      << " (" << selected_reason << "; largest stable tested "
                      << fmt_u64(largest_stable_bs) << ")\n";
        }
        return selected_bs;
    }

    std::vector<uint64_t> raw_candidates{
        std::max<uint64_t>(50'000ULL, base / 2ULL),
        base,
        static_cast<uint64_t>(static_cast<double>(base) * 1.5),
        base * 2ULL,
        base * 3ULL,
        base * 4ULL,
    };
    std::vector<uint64_t> candidates;
    candidates.reserve(raw_candidates.size());
    for (uint64_t v : raw_candidates) {
        if (v == 0) {
            continue;
        }
        candidates.push_back(std::max<uint64_t>(1, std::min<uint64_t>(cap, v)));
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.empty()) {
        return std::max<uint64_t>(1, std::min<uint64_t>(base, total_count));
    }

    if (verbose) {
        std::cout << "Auto batch finder (" << mode << "): testing " << fmt_size(candidates.size())
                  << " candidate(s)...\n";
    }
    uint64_t best_bs = candidates[0];
    double best_rate = -1.0;
    int tested = 0;
    for (uint64_t bs : candidates) {
        const std::optional<BatchProbeResult> result = benchmark_batch_candidate(run_probe, bs, false);
        if (!result.has_value()) {
            if (verbose) {
                std::cout << "- " << fmt_u64(bs) << ": OOM/failed (skipped)\n";
            }
            continue;
        }
        ++tested;
        if (verbose) {
            std::cout << "- " << fmt_u64(bs) << ": " << fmt_double(result->rate_sps, 0) << " seeds/sec\n";
        }
        if (result->rate_sps > best_rate) {
            best_rate = result->rate_sps;
            best_bs = bs;
        }
    }
    if (tested == 0) {
        const uint64_t fallback = std::max<uint64_t>(1, std::min<uint64_t>(base, cap));
        if (verbose) {
            std::cout << "Auto batch finder (" << mode << "): fallback to " << fmt_u64(fallback) << "\n";
        }
        return fallback;
    }
    if (verbose) {
        std::cout << "Auto batch finder (" << mode << "): selected " << fmt_u64(best_bs) << "\n";
    }
    return best_bs;
}

static uint64_t choose_batch_size(
    const CliArgs &args,
    bool strict_or_biome,
    uint64_t total_mem_bytes,
    uint64_t total_count,
    const std::function<void(uint64_t)> &run_probe,
    bool verbose,
    uint32_t min_async_memory_slots = 1U
) {
    if (args.batch_size != "balanced" && args.batch_size != "max") {
        const uint64_t manual = parse_u64(args.batch_size);
        if (manual == 0) {
            throw std::runtime_error("--batch-size integer must be > 0.");
        }
        if (manual > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error("--batch-size cannot exceed 4294967295 seeds.");
        }
        return std::min<uint64_t>(manual, total_count);
    }
    return auto_find_batch_size(
        args.batch_size,
        total_count,
        strict_or_biome,
        total_mem_bytes,
        run_probe,
        verbose,
        min_async_memory_slots);
}

static double benchmark_async_gpu_depth(
    const GpuFilterApi &gpu_backend,
    const GpuPrefilterPlan &gpu_prefilter_plan,
    uint64_t start_seed,
    uint64_t batch_size,
    uint32_t depth,
    uint32_t probe_batch_count = 0U
) {
    if (depth == 0U || batch_size == 0ULL) {
        return 0.0;
    }
    struct PendingGpuBatch {
        uint32_t slot_id = 0U;
        uint64_t start_seed = 0ULL;
        uint64_t count = 0ULL;
    };

    const uint32_t probe_batches = probe_batch_count == 0U
        ? std::max<uint32_t>(3U, depth + 1U)
        : std::max<uint32_t>(1U, probe_batch_count);
    std::deque<PendingGpuBatch> pending;
    std::vector<uint64_t> scratch;
    std::vector<uint64_t> cleanup_scratch;
    uint32_t next_slot = 0U;
    uint32_t submitted_batches = 0U;
    uint64_t next_start = start_seed;
    uint64_t processed = 0ULL;

    auto submit_one = [&]() -> bool {
        if (submitted_batches >= probe_batches || pending.size() >= depth) {
            return false;
        }
        const uint32_t slot_id = next_slot;
        gpu_backend.submit_batch(
            slot_id,
            next_start,
            batch_size,
            gpu_prefilter_plan.regions,
            gpu_prefilter_plan.constraints
        );
        pending.push_back(PendingGpuBatch{slot_id, next_start, batch_size});
        next_start = (next_start + batch_size) & JAVA_MASK;
        ++submitted_batches;
        next_slot = (next_slot + 1U) % depth;
        return true;
    };

    auto drain_pending = [&]() {
        while (!pending.empty()) {
            PendingGpuBatch pending_batch = pending.front();
            pending.pop_front();
            double kernel_seconds = 0.0;
            double transfer_seconds = 0.0;
            try {
                gpu_backend.collect_batch(
                    pending_batch.slot_id,
                    cleanup_scratch,
                    kernel_seconds,
                    transfer_seconds
                );
            } catch (...) {
            }
        }
    };

    const auto t0 = std::chrono::high_resolution_clock::now();
    try {
        while (pending.size() < depth && submit_one()) {
        }
        while (!pending.empty()) {
            PendingGpuBatch pending_batch = pending.front();
            pending.pop_front();
            double kernel_seconds = 0.0;
            double transfer_seconds = 0.0;
            gpu_backend.collect_batch(pending_batch.slot_id, scratch, kernel_seconds, transfer_seconds);
            (void)kernel_seconds;
            (void)transfer_seconds;
            processed += pending_batch.count;
            while (pending.size() < depth && submit_one()) {
            }
        }
    } catch (const std::bad_alloc &) {
        drain_pending();
        return 0.0;
    } catch (const std::runtime_error &exc) {
        const std::string msg = lower_ascii(exc.what());
        if (is_gpu_probe_resource_failure(msg)) {
            drain_pending();
            return 0.0;
        }
        drain_pending();
        throw;
    }
    const double dt = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
    return dt > 0.0 ? static_cast<double>(processed) / dt : 0.0;
}

static std::optional<BatchProbeResult> benchmark_mixed_async_candidate(
    const GpuFilterApi &gpu_backend,
    const GpuPrefilterPlan &gpu_prefilter_plan,
    uint64_t start_seed,
    uint64_t batch_size,
    uint32_t depth,
    bool sample_gpu_utilization,
    uint32_t probe_batch_count = 0U,
    bool run_warmup = true
) {
    try {
        NvidiaSmiSampler sampler(sample_gpu_utilization);
        if (run_warmup) {
            const uint32_t warmup_batches = probe_batch_count == 0U
                ? std::min<uint32_t>(3U, std::max<uint32_t>(1U, depth))
                : std::min<uint32_t>(probe_batch_count, 2U);
            const double warmup_rate = benchmark_async_gpu_depth(
                gpu_backend,
                gpu_prefilter_plan,
                start_seed,
                batch_size,
                depth,
                warmup_batches
            );
            if (warmup_rate <= 0.0) {
                sampler.stop();
                return std::nullopt;
            }
        }
        const double rate = benchmark_async_gpu_depth(
            gpu_backend,
            gpu_prefilter_plan,
            start_seed,
            batch_size,
            depth,
            probe_batch_count
        );
        sampler.stop();
        if (rate <= 0.0) {
            return std::nullopt;
        }
        BatchProbeResult result{};
        result.rate_sps = rate;
        result.has_gpu_sample = sampler.available();
        result.gpu_util_pct = sampler.max_gpu_util_pct();
        result.gpu_memory_used_pct = sampler.max_memory_used_pct();
        return result;
    } catch (const std::bad_alloc &) {
        return std::nullopt;
    } catch (const std::runtime_error &exc) {
        const std::string msg = lower_ascii(exc.what());
        if (is_gpu_probe_resource_failure(msg)) {
            return std::nullopt;
        }
        throw;
    }
}

static uint32_t max_async_slots_for_batch_memory(
    const GpuFilterApi &gpu_backend,
    uint64_t capped_batch_size,
    uint32_t available_slots
) {
    if (capped_batch_size == 0ULL || available_slots <= 1U) {
        return std::max<uint32_t>(1U, available_slots);
    }
    const uint64_t total_mem = gpu_backend.total_mem_bytes();
    if (total_mem == 0ULL) {
        return available_slots;
    }
    const long double per_slot_device_bytes =
        static_cast<long double>(capped_batch_size) * static_cast<long double>(sizeof(uint64_t));
    if (per_slot_device_bytes <= 0.0L) {
        return available_slots;
    }
    const long double budget = static_cast<long double>(total_mem) * 0.72L;
    uint64_t slots_by_memory = static_cast<uint64_t>(budget / per_slot_device_bytes);
    if (slots_by_memory == 0ULL) {
        slots_by_memory = 1ULL;
    }
    return std::max<uint32_t>(
        1U,
        std::min<uint32_t>(available_slots, static_cast<uint32_t>(std::min<uint64_t>(slots_by_memory, 8ULL))));
}

static uint32_t choose_gpu_inflight_batches(
    const CliArgs &args,
    const GpuFilterApi &gpu_backend,
    const GpuPrefilterPlan &gpu_prefilter_plan,
    uint64_t start_seed,
    uint64_t total_count,
    uint64_t capped_batch_size,
    bool use_async_gpu_pipeline,
    bool verbose
) {
    if (!use_async_gpu_pipeline || gpu_prefilter_plan.constraints.empty()) {
        return 0U;
    }
    const uint32_t max_slots = max_async_slots_for_batch_memory(
        gpu_backend,
        capped_batch_size,
        std::max<uint32_t>(1U, gpu_backend.async_max_slots()));
    if (args.gpu_inflight_batches != "auto") {
        const uint32_t requested = static_cast<uint32_t>(parse_u64(args.gpu_inflight_batches));
        return std::max<uint32_t>(1U, std::min<uint32_t>(requested, max_slots));
    }
    const uint32_t target_baseline =
        (args.scan_mode == "mixed" && max_slots >= 4U) ? 4U : 2U;
    const uint32_t baseline = std::min<uint32_t>(target_baseline, max_slots);
    const uint64_t min_probe_count = args.scan_mode == "mixed" ? 8'000'000ULL : 32'000'000ULL;
    if (max_slots <= baseline || total_count < min_probe_count || capped_batch_size == 0ULL) {
        return baseline;
    }

    const uint64_t probe_batch_size = std::min<uint64_t>(
        capped_batch_size,
        args.scan_mode == "mixed" ? 16'000'000ULL : 8'000'000ULL);
    std::vector<uint32_t> candidates{1U, 2U, 3U, 4U, 6U, 8U};
    uint32_t best_depth = baseline;
    double baseline_rate = 0.0;
    double best_rate = 0.0;

    for (uint32_t candidate : candidates) {
        if (candidate > max_slots) {
            continue;
        }
        double rate = 0.0;
        try {
            rate = benchmark_async_gpu_depth(
                gpu_backend,
                gpu_prefilter_plan,
                start_seed,
                probe_batch_size,
                candidate
            );
        } catch (const std::bad_alloc &) {
            rate = 0.0;
        } catch (const std::runtime_error &exc) {
            const std::string msg = lower_ascii(exc.what());
            if (!is_gpu_probe_resource_failure(msg)) {
                throw;
            }
            rate = 0.0;
        }
        if (verbose) {
            std::cout << "GPU async depth probe " << fmt_u64(candidate)
                      << ": " << fmt_double(rate, 0) << " seeds/sec\n";
        }
        if (candidate == baseline) {
            baseline_rate = rate;
        }
        if (rate > best_rate) {
            best_rate = rate;
            best_depth = candidate;
        }
    }

    if (baseline_rate <= 0.0) {
        return baseline;
    }
    if (best_depth != baseline && best_rate >= baseline_rate * 1.03) {
        return best_depth;
    }
    return baseline;
}

static MixedAsyncTuningChoice choose_mixed_async_max_batch_and_depth(
    const CliArgs &args,
    const GpuFilterApi &gpu_backend,
    const GpuPrefilterPlan &gpu_prefilter_plan,
    uint64_t start_seed,
    uint64_t total_count,
    bool verbose
) {
    MixedAsyncTuningChoice choice{};
    if (args.batch_size != "max" || args.scan_mode != "mixed" || gpu_prefilter_plan.constraints.empty()) {
        return choice;
    }

    const uint64_t total_mem_bytes = gpu_backend.total_mem_bytes();
    const uint64_t base = recommended_batch_size(1, total_count, false, total_mem_bytes);
    const uint64_t cap = std::min<uint64_t>(
        total_count,
        std::max<uint64_t>(base, estimate_max_probe_batch_cap(false, total_mem_bytes)));
    if (cap == 0ULL) {
        return choice;
    }

    const uint32_t available_slots = std::max<uint32_t>(1U, gpu_backend.async_max_slots());
    const uint64_t gate_product = gpu_prefilter_gate_product(gpu_prefilter_plan);
    const bool has_surrogate_gate = gate_product > 1ULL;
    if (has_surrogate_gate && args.gpu_inflight_batches != "auto") {
        const uint64_t target_gated_batch =
            gate_product >= 4096ULL ? 8'000'000ULL : 32'000'000ULL;
        const uint64_t gated_batch_size = std::max<uint64_t>(
            1ULL,
            std::min<uint64_t>(cap, target_gated_batch));
        const uint32_t requested_depth =
            static_cast<uint32_t>(std::max<uint64_t>(1ULL, parse_u64(args.gpu_inflight_batches)));
        const uint32_t gated_depth = std::max<uint32_t>(
            1U,
            std::min<uint32_t>(
                requested_depth,
                max_async_slots_for_batch_memory(gpu_backend, gated_batch_size, available_slots)));
        choice.valid = true;
        choice.batch_size = gated_batch_size;
        choice.depth = gated_depth;
        choice.rate_sps = 0.0;
        if (verbose) {
            std::cout << "Mixed async max tuner: strict surrogate gate active; selected batch="
                      << fmt_u64(choice.batch_size)
                      << ", depth=" << fmt_u64(choice.depth)
                      << " without extended probes.\n";
        }
        return choice;
    }
    std::vector<uint64_t> candidates;
    auto push_mixed_candidate = [&](uint64_t value) {
        if (value == 0ULL || cap == 0ULL) {
            return;
        }
        candidates.push_back(std::max<uint64_t>(1ULL, std::min<uint64_t>(value, cap)));
    };
    push_mixed_candidate(base);
    const uint64_t mixed_probe_points[] = {
        8'000'000ULL,
        16'000'000ULL,
        32'000'000ULL,
        64'000'000ULL,
        96'000'000ULL,
        128'000'000ULL,
        192'000'000ULL,
        256'000'000ULL,
        384'000'000ULL,
        512'000'000ULL,
        768'000'000ULL,
        1'000'000'000ULL,
    };
    for (uint64_t point : mixed_probe_points) {
        push_mixed_candidate(point);
    }
    push_mixed_candidate(cap);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (cap >= 8'000'000ULL) {
        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [](uint64_t candidate) { return candidate < 8'000'000ULL; }),
            candidates.end());
    }
    std::vector<MixedAsyncProbeRecord> records;
    records.reserve(candidates.size() * 2U);

    auto push_unique_depth = [](std::vector<uint32_t> &depths, uint32_t depth) {
        if (depth == 0U) {
            return;
        }
        if (std::find(depths.begin(), depths.end(), depth) == depths.end()) {
            depths.push_back(depth);
        }
    };

    auto all_depth_candidates = [&](uint64_t batch_size) {
        std::vector<uint32_t> depths;
        const uint32_t max_slots = max_async_slots_for_batch_memory(gpu_backend, batch_size, available_slots);
        if (args.gpu_inflight_batches != "auto") {
            const uint32_t requested = static_cast<uint32_t>(parse_u64(args.gpu_inflight_batches));
            push_unique_depth(depths, std::max<uint32_t>(1U, std::min<uint32_t>(requested, max_slots)));
            return depths;
        }
        const uint32_t raw_depths[] = {1U, 2U, 3U, 4U, 6U, 8U};
        for (uint32_t depth : raw_depths) {
            if (depth <= max_slots) {
                push_unique_depth(depths, depth);
            }
        }
        return depths;
    };

    auto has_record = [&](uint64_t batch_size, uint32_t depth) {
        return std::any_of(records.begin(), records.end(), [&](const MixedAsyncProbeRecord &record) {
            return record.batch_size == batch_size && record.depth == depth;
        });
    };

    auto print_probe = [&](uint64_t batch_size, uint32_t depth, const BatchProbeResult *result, bool failed) {
        if (!verbose) {
            return;
        }
        std::cout << "- batch=" << fmt_u64(batch_size)
                  << ", depth=" << fmt_u64(depth)
                  << ": ";
        if (failed || result == nullptr) {
            std::cout << "OOM/failed\n";
            return;
        }
        std::cout << fmt_double(result->rate_sps, 0) << " seeds/sec";
        if (result->has_gpu_sample) {
            std::cout << ", gpu=" << fmt_double(result->gpu_util_pct, 1)
                      << "%, vram=" << fmt_double(result->gpu_memory_used_pct, 1) << "%";
        } else {
            std::cout << ", gpu=nvidia-smi unavailable";
        }
        std::cout << "\n";
    };

    auto measure = [&](uint64_t batch_size, uint32_t depth) -> bool {
        if (depth == 0U || has_record(batch_size, depth)) {
            return false;
        }
        const uint32_t quick_probe_batches = args.gpu_inflight_batches == "auto"
            ? std::min<uint32_t>(5U, std::max<uint32_t>(3U, depth + 1U))
            : 1U;
        const std::optional<BatchProbeResult> result = benchmark_mixed_async_candidate(
            gpu_backend,
            gpu_prefilter_plan,
            start_seed,
            batch_size,
            depth,
            false,
            quick_probe_batches,
            false
        );
        if (!result.has_value()) {
            print_probe(batch_size, depth, nullptr, true);
            return false;
        }
        MixedAsyncProbeRecord record{};
        record.batch_size = batch_size;
        record.depth = depth;
        record.result = *result;
        records.push_back(record);
        print_probe(batch_size, depth, &record.result, false);
        return true;
    };

    if (verbose) {
        std::cout << "Mixed async max tuner: probing " << fmt_size(candidates.size())
                  << " batch candidate(s) up to " << fmt_u64(cap)
                  << " with up to " << fmt_u64(available_slots)
                  << " async slot(s); selecting by measured throughput...\n";
    }

    int fail_count = 0;
    const int fail_limit = 3;
    uint64_t largest_stable_batch = 0ULL;
    const uint32_t min_mixed_probe_depth = std::min<uint32_t>(4U, available_slots);
    for (uint64_t batch_size : candidates) {
        const std::vector<uint32_t> depths = all_depth_candidates(batch_size);
        if (depths.empty()) {
            continue;
        }
        if (args.gpu_inflight_batches != "auto" && min_mixed_probe_depth > 1U &&
            *std::max_element(depths.begin(), depths.end()) < min_mixed_probe_depth) {
            continue;
        }
        std::vector<uint32_t> initial_depths;
        push_unique_depth(initial_depths, depths.front());

        bool any_ok = false;
        for (uint32_t depth : initial_depths) {
            any_ok = measure(batch_size, depth) || any_ok;
        }
        if (any_ok) {
            largest_stable_batch = batch_size;
            fail_count = 0;
        } else {
            ++fail_count;
            if (fail_count >= fail_limit) {
                if (verbose) {
                    std::cout << "Mixed async max tuner: fail limit reached; stopping batch probes.\n";
                }
                break;
            }
        }
    }

    if (records.empty()) {
        return choice;
    }

    std::vector<uint64_t> depth_tune_batches;
    auto push_unique_batch = [&](uint64_t batch_size) {
        if (batch_size == 0ULL) {
            return;
        }
        if (std::find(depth_tune_batches.begin(), depth_tune_batches.end(), batch_size) == depth_tune_batches.end()) {
            depth_tune_batches.push_back(batch_size);
        }
    };

    std::vector<MixedAsyncProbeRecord> sorted_records = records;
    std::sort(sorted_records.begin(), sorted_records.end(), [](const auto &a, const auto &b) {
        return a.result.rate_sps > b.result.rate_sps;
    });
    for (const MixedAsyncProbeRecord &record : sorted_records) {
        push_unique_batch(record.batch_size);
        if (depth_tune_batches.size() >= 3U) {
            break;
        }
    }
    push_unique_batch(largest_stable_batch);

    if (args.gpu_inflight_batches == "auto") {
        for (uint64_t batch_size : depth_tune_batches) {
            const std::vector<uint32_t> depths = all_depth_candidates(batch_size);
            for (uint32_t depth : depths) {
                if (!measure(batch_size, depth) && depth > 1U) {
                    break;
                }
            }
        }
    }

    std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
        return a.result.rate_sps > b.result.rate_sps;
    });
    double best_verified_rate = -1.0;
    uint32_t verified_count = 0U;
    uint32_t verified_attempts = 0U;
    const uint32_t max_verified_records = args.gpu_inflight_batches == "auto" ? 6U : 1U;
    const uint32_t max_verified_attempts = args.gpu_inflight_batches == "auto" ? 12U : 4U;
    uint64_t mixed_validation_select_cap = 96'000'000ULL;
    if (args.mixed_queue_capacity >= 8'000'000ULL) {
        mixed_validation_select_cap = 384'000'000ULL;
    } else if (args.mixed_queue_capacity >= 4'000'000ULL) {
        mixed_validation_select_cap = 192'000'000ULL;
    }
    for (const MixedAsyncProbeRecord &record : records) {
        if (verified_count >= max_verified_records || verified_attempts >= max_verified_attempts) {
            break;
        }
        if (record.batch_size > mixed_validation_select_cap) {
            continue;
        }
        ++verified_attempts;
        const uint32_t verify_probe_batches = args.gpu_inflight_batches == "auto"
            ? std::min<uint32_t>(7U, std::max<uint32_t>(3U, record.depth + 1U))
            : 3U;
        const std::optional<BatchProbeResult> verified = benchmark_mixed_async_candidate(
            gpu_backend,
            gpu_prefilter_plan,
            start_seed,
            record.batch_size,
            record.depth,
            false,
            verify_probe_batches,
            true
        );
        if (!verified.has_value()) {
            if (verbose) {
                std::cout << "Mixed async max tuner: verification failed for batch="
                          << fmt_u64(record.batch_size)
                          << ", depth=" << fmt_u64(record.depth) << "\n";
            }
            continue;
        }
        ++verified_count;
        if (verified->rate_sps > best_verified_rate) {
            best_verified_rate = verified->rate_sps;
            choice.valid = true;
            choice.batch_size = record.batch_size;
            choice.depth = record.depth;
            choice.rate_sps = verified->rate_sps;
        }
    }
    if (!choice.valid) {
        const auto best_it = std::max_element(records.begin(), records.end(), [](const auto &a, const auto &b) {
            return a.result.rate_sps < b.result.rate_sps;
        });
        if (best_it == records.end()) {
            return choice;
        }
        choice.valid = true;
        choice.batch_size = best_it->batch_size;
        choice.depth = best_it->depth;
        choice.rate_sps = best_it->result.rate_sps;
    }
    if (verbose) {
        std::cout << "Mixed async max tuner: selected batch=" << fmt_u64(choice.batch_size)
                  << ", depth=" << fmt_u64(choice.depth)
                  << " (" << fmt_double(choice.rate_sps, 0)
                  << " seeds/sec; largest stable tested " << fmt_u64(largest_stable_batch) << ")\n";
    }
    return choice;
}

static uint32_t choose_effective_validation_workers(uint32_t max_workers, uint64_t hit_count) {
    if (max_workers <= 1U || hit_count <= 128ULL) {
        return 1U;
    }
    const uint32_t chunk_target = 512U;
    const uint32_t by_work = static_cast<uint32_t>((hit_count + chunk_target - 1ULL) / chunk_target);
    uint32_t eff = std::max<uint32_t>(1U, std::min<uint32_t>(max_workers, by_work));
    if (hit_count < 1024ULL) {
        eff = std::min<uint32_t>(eff, 4U);
    }
    return std::max<uint32_t>(1U, eff);
}

class BoundedSurvivorBatchQueue {
public:
    explicit BoundedSurvivorBatchQueue(uint64_t capacity, size_t max_batch_size)
        : capacity_(std::max<uint64_t>(1ULL, capacity)),
          max_batch_size_(std::max<size_t>(1U, max_batch_size)) {}

    bool try_push_batch(
        std::vector<uint64_t> &&batch,
        const std::atomic<bool> &cancelled,
        uint64_t &dropped_count,
        uint64_t &queue_full_events,
        uint64_t &push_wait_ns,
        bool allow_drop
    ) {
        if (batch.empty()) {
            return true;
        }
        const auto wait_start = std::chrono::high_resolution_clock::now();
        size_t offset = 0U;
        std::unique_lock<std::mutex> lock(mu_);
        if (closed_ || cancelled.load()) {
            return false;
        }
        if (!allow_drop) {
            while (offset < batch.size()) {
                not_full_.wait(lock, [&]() { return closed_ || cancelled.load() || size_ < capacity_; });
                if (closed_ || cancelled.load()) {
                    const auto wait_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now() - wait_start);
                    push_wait_ns += static_cast<uint64_t>(std::max<int64_t>(0, wait_elapsed.count()));
                    return false;
                }
                const uint64_t available = capacity_ - static_cast<uint64_t>(size_);
                const size_t take = static_cast<size_t>(
                    std::min<uint64_t>(
                        available,
                        static_cast<uint64_t>(balanced_chunk_size(batch.size() - offset))));
                std::vector<uint64_t> chunk;
                chunk.reserve(take);
                chunk.insert(
                    chunk.end(),
                    std::make_move_iterator(batch.begin() + static_cast<std::ptrdiff_t>(offset)),
                    std::make_move_iterator(batch.begin() + static_cast<std::ptrdiff_t>(offset + take)));
                offset += take;
                size_ += chunk.size();
                batches_.push_back(std::move(chunk));
                not_empty_.notify_all();
            }
            const auto wait_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - wait_start);
            push_wait_ns += static_cast<uint64_t>(std::max<int64_t>(0, wait_elapsed.count()));
            return true;
        }
        const auto wait_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - wait_start);
        push_wait_ns += static_cast<uint64_t>(std::max<int64_t>(0, wait_elapsed.count()));
        const uint64_t current = static_cast<uint64_t>(size_);
        const uint64_t available = current >= capacity_ ? 0ULL : capacity_ - current;
        if (available == 0ULL) {
            ++queue_full_events;
            dropped_count += static_cast<uint64_t>(batch.size());
            return true;
        }
        if (static_cast<uint64_t>(batch.size()) > available) {
            ++queue_full_events;
            dropped_count += static_cast<uint64_t>(batch.size()) - available;
            batch.resize(static_cast<size_t>(available));
        }
        while (offset < batch.size()) {
            const size_t take = balanced_chunk_size(batch.size() - offset);
            std::vector<uint64_t> chunk;
            chunk.reserve(take);
            chunk.insert(
                chunk.end(),
                std::make_move_iterator(batch.begin() + static_cast<std::ptrdiff_t>(offset)),
                std::make_move_iterator(batch.begin() + static_cast<std::ptrdiff_t>(offset + take)));
            offset += take;
            size_ += chunk.size();
            batches_.push_back(std::move(chunk));
        }
        not_empty_.notify_all();
        return true;
    }

    bool pop_batch(std::vector<uint64_t> &out) {
        std::unique_lock<std::mutex> lock(mu_);
        not_empty_.wait(lock, [&]() { return closed_ || !batches_.empty(); });
        if (batches_.empty()) {
            return false;
        }
        out = std::move(batches_.front());
        batches_.pop_front();
        size_ -= out.size();
        not_full_.notify_all();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    uint64_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return static_cast<uint64_t>(size_);
    }

    uint64_t capacity() const {
        return capacity_;
    }

    double saturation() const {
        return static_cast<double>(size()) / static_cast<double>(capacity_);
    }

private:
    size_t balanced_chunk_size(size_t remaining) const {
        if (remaining <= max_batch_size_) {
            return remaining;
        }
        const size_t chunks_left = (remaining + max_batch_size_ - 1U) / max_batch_size_;
        return (remaining + chunks_left - 1U) / chunks_left;
    }

    const uint64_t capacity_;
    const size_t max_batch_size_;
    mutable std::mutex mu_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<std::vector<uint64_t>> batches_;
    size_t size_ = 0;
    bool closed_ = false;
};

class StdinController {
public:
    explicit StdinController(
        bool enabled,
        const CliArgs &args,
        std::vector<uint64_t> &printed,
        std::atomic<bool> &terminal_mode
    )
        : enabled_(enabled), args_(args), printed_(printed), terminal_mode_(terminal_mode) {
        if (!enabled_) {
            return;
        }
        thread_ = std::thread([this]() { this->worker(); });
    }

    ~StdinController() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool paused() const { return paused_.load(); }

private:
    bool enabled_ = false;
    const CliArgs &args_;
    std::vector<uint64_t> &printed_;
    std::atomic<bool> &terminal_mode_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;

    void print_snapshot() const {
        if (!terminal_mode_.load()) {
            return;
        }
        std::cout << "\n=== EARLY SEED SNAPSHOT ===\n";
        if (printed_.empty()) {
            std::cout << "(no seeds yet)\n";
        } else {
            for (uint64_t seed : printed_) {
                const uint64_t lifted = lift_seed_to_64(seed, args_.upper16);
                if (args_.output_mode == "raw") {
                    std::cout << fmt_seed(seed) << "\n";
                } else if (args_.output_mode == "lift64") {
                    std::cout << fmt_seed(lifted) << "\n";
                } else {
                    std::cout << "raw=" << fmt_seed(seed) << "  lift64=" << fmt_seed(lifted) << "\n";
                }
            }
        }
        std::cout << "==========================\n";
    }

    bool stdin_has_pending_input() const {
#if defined(_WIN32)
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            const DWORD ftype = GetFileType(h);
            if (ftype == FILE_TYPE_PIPE) {
                DWORD avail = 0;
                if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) != 0) {
                    return avail > 0;
                }
            } else if (ftype == FILE_TYPE_CHAR) {
                DWORD events = 0;
                if (GetNumberOfConsoleInputEvents(h, &events) != 0) {
                    return events > 0;
                }
            }
        }
#else
        struct pollfd pfd {};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        const int rc = poll(&pfd, 1, 0);
        if (rc > 0 && (pfd.revents & POLLIN) != 0) {
            return true;
        }
#endif
        std::streambuf *buf = std::cin.rdbuf();
        if (buf == nullptr) {
            return false;
        }
        return buf->in_avail() > 0;
    }

    void worker() {
        std::string line;
        while (!stop_.load()) {
            if (!stdin_has_pending_input()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (!std::getline(std::cin, line)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            const std::string cmd = lower_ascii(trim(line));
            if (cmd == "pause" || cmd == "p") {
                paused_.store(true);
                print_snapshot();
                if (terminal_mode_.load()) {
                    std::cout << "[Control] Paused by command.\n";
                }
            } else if (cmd == "resume" || cmd == "r") {
                paused_.store(false);
                if (terminal_mode_.load()) {
                    std::cout << "[Control] Resuming cracking...\n";
                }
            } else if (cmd == "toggle" || cmd == "t") {
                const bool now = !paused_.load();
                paused_.store(now);
                if (now) {
                    print_snapshot();
                    if (terminal_mode_.load()) {
                        std::cout << "[Control] Paused by command.\n";
                    }
                } else {
                    if (terminal_mode_.load()) {
                        std::cout << "[Control] Resuming cracking...\n";
                    }
                }
            } else if (cmd == "snapshot" || cmd == "s") {
                print_snapshot();
            } else if (cmd == "terminal_on" || cmd == "term_on") {
                terminal_mode_.store(true);
                std::cout << "[Control] Terminal mode enabled.\n";
            } else if (cmd == "terminal_off" || cmd == "term_off") {
                if (terminal_mode_.load()) {
                    std::cout << "[Control] Terminal mode disabled.\n";
                }
                terminal_mode_.store(false);
            } else if (cmd == "terminal_toggle" || cmd == "terminal" || cmd == "term") {
                const bool now = !terminal_mode_.load();
                terminal_mode_.store(now);
                if (now) {
                    std::cout << "[Control] Terminal mode enabled.\n";
                }
            }
        }
    }
};

static void print_supported_structures() {
    std::cout << "Placement-mode preset structures:\n";
    std::vector<std::string> names;
    names.reserve(kStructurePresets.size());
    for (const auto &kv : kStructurePresets) {
        names.push_back(kv.first);
    }
    std::sort(names.begin(), names.end());
    for (const std::string &name : names) {
        const StructurePreset &p = kStructurePresets.at(name);
        std::cout << "- " << name << ": spacing=" << fmt_i64(p.spacing) << ", separation=" << fmt_i64(p.separation)
                  << ", salt=" << fmt_i64(p.salt) << ", spread="
                  << (p.spread_type == SCAN_SPREAD_TRIANGULAR ? "triangular" : "linear")
                  << "\n";
    }
    std::cout << "Placement mode (direct native scan):\n";
    for (const auto &kv : kStructureIds) {
        if (kStructurePresets.find(kv.first) == kStructurePresets.end() && supports_direct_placement_mode(kv.first)) {
            std::cout << "- " << kv.first << "\n";
        }
    }
    std::cout << "- stronghold (native cubiomes ring iterator)\n";
    std::cout << "Strict-only structures:\n";
    std::cout << "Quad variants (strict, min_required=4):\n";
    std::cout << "- quad_hut (swamp_hut)\n";
    std::cout << "- quad_monument (ocean_monument)\n";
    std::cout << "- quad_desert_pyramid\n";
    std::cout << "- quad_igloo\n";
    std::cout << "- quad_jungle_temple\n";
}

static void print_query_template() {
    std::cout
        << "{\n"
        << "  \"version\": 1,\n"
        << "  \"logic\": \"and\",\n"
        << "  \"anchor\": {\"type\": \"origin\"},\n"
        << "  \"constraints\": [\n"
        << "    {\"id\":\"v1\",\"structure\":\"village\",\"mode\":\"strict\",\"within\":{\"anchor\":\"origin\",\"radius\":50}},\n"
        << "    {\"id\":\"rp1\",\"structure\":\"ruined_portal\",\"mode\":\"strict\",\"within\":{\"anchor\":\"fixed:128,-64\",\"radius\":60}}\n"
        << "  ],\n"
        << "  \"biome_filters\": [\n"
        << "    {\n"
        << "      \"y\": 64,\n"
        << "      \"radius\": 24,\n"
        << "      \"allowed\": [\"plains\"],\n"
        << "      \"shape\": {\"type\":\"square\",\"center\":[0,0],\"radius\":192,\"edge\":\"outward\"}\n"
        << "    }\n"
        << "  ],\n"
        << "  \"loot_filters\": [],\n"
        << "  \"sculpt\": {\n"
        << "    \"enabled\": false,\n"
        << "    \"pattern\": \"disk\",\n"
        << "    \"edge\": \"outward\",\n"
        << "    \"center\": \"origin\",\n"
        << "    \"radius\": 128,\n"
        << "    \"step\": 16,\n"
        << "    \"allowed\": [\"cherry_grove\"],\n"
        << "    \"dominance_threshold\": 0.85\n"
        << "  },\n"
        << "  \"output\": {\"detail\":\"matched_set\"},\n"
        << "  \"safety\": {\"completeness\":\"exact\"},\n"
        << "  \"performance\": {\"strict_workers\":0,\"candidate_cap_mode\":\"none\",\"strict_surrogate\":\"off\",\"execution_mode\":\"auto\",\"compiled_stage_a_mode\":\"auto\"}\n"
        << "}\n";
}

struct StageAConstraintGroup {
    std::vector<std::string> independent;
    std::vector<std::string> dependent;
    std::vector<std::string> reject_only;
    std::vector<std::string> stage_b_only;
};

struct PlanDiagnosticsSummary {
    StageAConstraintGroup groups;
    std::vector<std::string> chosen_drivers;
    std::vector<std::string> target_caps;
    double predicted_score = 0.0;
    double measured_single_driver_pass_rate = -1.0;
    double measured_selected_pass_rate = -1.0;
    std::string completeness_policy = "exact";
    std::string compiled_stage_a_mode = "auto";
    std::string route_decision = "gpu";
    std::string route_reason = "auto";
    bool compiled_stats_available = false;
    bool gpu_stage_a_enabled = true;
    double observed_stage_a_pass_rate = -1.0;
    double observed_stage_b_reject_rate = -1.0;
};

struct RunBatchStats {
    uint64_t input_seed_count = 0;
    uint64_t stage_a_survivor_count = 0;
    uint64_t stage_a5_survivor_count = 0;
    uint64_t stage_b_structure_survivor_count = 0;
    uint64_t stage_c_biome_survivor_count = 0;
    uint64_t final_valid_count = 0;
    uint64_t stage_a_reject_count = 0;
    uint64_t stage_a5_reject_count = 0;
    uint64_t stage_b_exact_reject_count = 0;
    uint64_t stage_c_biome_reject_count = 0;
    uint64_t mismatches = 0;
    uint64_t biome_rejected = 0;
    uint64_t sculpt_rejected = 0;
    uint64_t loot_rejected = 0;
    uint64_t cap_pruned_total = 0;
    double gpu_kernel_seconds = 0.0;
    double gpu_transfer_compaction_seconds = 0.0;
    double cpu_stage_a5_seconds = 0.0;
    double gpu_seconds = 0.0;
    double cpu_stage_b_seconds = 0.0;
    double cpu_stage_c_seconds = 0.0;
    double total_seconds = 0.0;
    ScannerCompiledBatchStats core_stats{};
    bool core_stats_available = false;
};

struct DetailSnapshot {
    int rc = 0;
    std::vector<NativeQueryMatchOut> matches;
    std::vector<NativeQueryBiomeOut> biomes;
    uint32_t cap_pruned = 0;
};

static bool is_stage_a_independent_candidate(const NativeQueryConstraintDesc &desc) {
    return desc.is_stronghold == 0U && desc.prefilter_supported != 0U && desc.attempt_anchor_exact_stage0 != 0U &&
           desc.anchor_type != SCAN_ANCHOR_DEP;
}

static bool is_stage_a_dependent_candidate(const NativeQueryConstraintDesc &desc) {
    return desc.is_stronghold == 0U && desc.prefilter_supported != 0U && desc.attempt_anchor_exact_stage0 != 0U &&
           desc.anchor_type == SCAN_ANCHOR_DEP;
}

static bool is_stage_a_reject_only_candidate(const NativeQueryConstraintDesc &desc) {
    return desc.is_stronghold == 0U && desc.prefilter_supported != 0U && desc.attempt_anchor_exact_stage0 == 0U;
}

static bool is_stage_b_only_candidate(const NativeQueryConstraintDesc &desc) {
    return !is_stage_a_independent_candidate(desc) && !is_stage_a_dependent_candidate(desc) &&
           !is_stage_a_reject_only_candidate(desc);
}

static std::vector<std::string> ordered_stage_a_driver_candidates(
    const QueryRuntime &runtime,
    const CompiledQueryPlanContext &query_plan,
    const std::unordered_map<std::string, int> &constraint_index
) {
    struct Candidate {
        std::string id;
        uint32_t candidate_cap = 0U;
        uint32_t min_required = 0U;
        uint32_t region_count = 0U;
        uint32_t quad_max_span = 0U;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(runtime.ordered_constraint_ids.size());
    for (const std::string &cid : runtime.ordered_constraint_ids) {
        const auto it_rt = runtime.constraints.find(cid);
        const auto it_idx = constraint_index.find(cid);
        if (it_rt == runtime.constraints.end() || it_idx == constraint_index.end()) {
            continue;
        }
        const NativeQueryConstraintDesc &desc = query_plan.constraints[static_cast<size_t>(it_idx->second)];
        if (!is_stage_a_independent_candidate(desc)) {
            continue;
        }
        candidates.push_back(Candidate{
            cid,
            desc.candidate_cap,
            desc.min_required,
            desc.region_count,
            desc.quad_max_span,
        });
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &lhs, const Candidate &rhs) {
        if (lhs.candidate_cap != rhs.candidate_cap) {
            return lhs.candidate_cap < rhs.candidate_cap;
        }
        if (lhs.region_count != rhs.region_count) {
            return lhs.region_count < rhs.region_count;
        }
        if (lhs.min_required != rhs.min_required) {
            return lhs.min_required > rhs.min_required;
        }
        if (lhs.quad_max_span != rhs.quad_max_span) {
            return lhs.quad_max_span < rhs.quad_max_span;
        }
        return lhs.id < rhs.id;
    });

    std::vector<std::string> out;
    out.reserve(candidates.size());
    for (const Candidate &c : candidates) {
        out.push_back(c.id);
    }
    return out;
}

struct StageADriverMeasurement {
    std::vector<std::string> drivers;
    uint64_t survivor_count = std::numeric_limits<uint64_t>::max();
    double pass_rate = std::numeric_limits<double>::infinity();
    bool measured = false;
};

struct StageARoutePlan {
    GpuPrefilterPlan active_gpu_plan;
    GpuPrefilterPlan multi_gpu_plan;
    std::vector<std::string> chosen_drivers;
    std::vector<std::string> best_single_drivers;
    std::vector<std::string> best_multi_drivers;
    double measured_single_driver_pass_rate = -1.0;
    double measured_multi_driver_pass_rate = -1.0;
    double measured_selected_pass_rate = -1.0;
    std::string route_decision = "gpu_off";
    std::string route_reason = "disabled";
    bool gpu_enabled = false;
};

static bool gpu_stage_a_is_final_placement_filter(
    const QueryRuntime &runtime,
    const CompiledQueryPlanContext &query_plan,
    const StageARoutePlan &route,
    bool sculpt_enabled,
    bool loot_enabled,
    bool structure_settings_enabled
) {
    if (!route.gpu_enabled || sculpt_enabled || loot_enabled || structure_settings_enabled || !query_plan.biome_filters.empty()) {
        return false;
    }
    if (runtime.constraints.empty() || route.chosen_drivers.size() != runtime.constraints.size() ||
        route.active_gpu_plan.constraints.size() != runtime.constraints.size()) {
        return false;
    }

    std::unordered_set<std::string> chosen(route.chosen_drivers.begin(), route.chosen_drivers.end());
    for (const std::string &cid : runtime.ordered_constraint_ids) {
        const auto it = runtime.constraints.find(cid);
        if (it == runtime.constraints.end()) {
            return false;
        }
        const QueryConstraintRuntime &rc = it->second;
        int32_t fixed_x = 0;
        int32_t fixed_z = 0;
        if (chosen.find(cid) == chosen.end() || rc.spec.mode != "placement" || rc.spec.structure == "stronghold" ||
            (rc.spec.anchor != "origin" && !parse_fixed_anchor_coords(rc.spec.anchor, fixed_x, fixed_z)) ||
            !rc.prefilter_supported || rc.context.regions.empty()) {
            return false;
        }
    }
    return true;
}

static bool stage_a_measurement_better(
    const StageADriverMeasurement &lhs,
    const StageADriverMeasurement &rhs
) {
    if (lhs.survivor_count != rhs.survivor_count) {
        return lhs.survivor_count < rhs.survivor_count;
    }
    if (lhs.drivers.size() != rhs.drivers.size()) {
        return lhs.drivers.size() < rhs.drivers.size();
    }
    return lhs.drivers < rhs.drivers;
}

static StageADriverMeasurement measure_stage_a_driver_combo(
    const GpuFilterApi &gpu_backend,
    uint64_t start_seed,
    uint64_t sample_count,
    const QueryRuntime &runtime,
    const std::vector<std::string> &drivers
) {
    StageADriverMeasurement measurement{};
    measurement.drivers = drivers;
    if (drivers.empty()) {
        measurement.survivor_count = sample_count;
        measurement.pass_rate = sample_count == 0U ? 0.0 : 1.0;
        measurement.measured = true;
        return measurement;
    }

    std::unordered_set<std::string> selected_ids;
    selected_ids.reserve(drivers.size());
    for (const std::string &driver : drivers) {
        selected_ids.insert(driver);
    }
    const GpuPrefilterPlan plan = build_gpu_prefilter_plan(runtime, &selected_ids);
    if (!gpu_prefilter_plan_fits_const_region_buffer(plan)) {
        measurement.measured = false;
        return measurement;
    }
    std::vector<uint64_t> survivors;
    gpu_backend.filter_multi_into(start_seed, sample_count, plan.regions, plan.constraints, survivors);
    measurement.survivor_count = static_cast<uint64_t>(survivors.size());
    measurement.pass_rate =
        sample_count == 0U ? 0.0 : static_cast<double>(measurement.survivor_count) / static_cast<double>(sample_count);
    measurement.measured = true;
    return measurement;
}

static std::vector<std::string> choose_static_stage_a_drivers(
    const std::vector<std::string> &driver_candidates,
    CompiledStageAMode mode
) {
    if (driver_candidates.empty()) {
        return {};
    }
    size_t driver_count = 1U;
    if (mode == CompiledStageAMode::multi) {
        driver_count = driver_candidates.size();
    }
    return std::vector<std::string>(driver_candidates.begin(), driver_candidates.begin() + driver_count);
}

static StageARoutePlan choose_stage_a_route_plan(
    const QueryRuntime &runtime,
    const CompiledQueryPlanContext &query_plan,
    const std::unordered_map<std::string, int> &constraint_index,
    CompiledStageAMode mode,
    const GpuFilterApi *gpu_backend,
    uint64_t start_seed,
    uint64_t total_count
) {
    StageARoutePlan route{};
    const std::vector<std::string> driver_candidates =
        ordered_stage_a_driver_candidates(runtime, query_plan, constraint_index);

    if (mode == CompiledStageAMode::gpu_off) {
        route.route_decision = "gpu_off";
        route.route_reason = "forced_gpu_off";
        return route;
    }
    if (mode == CompiledStageAMode::cpu_a5) {
        route.route_decision = "cpu_stage_a5_heavy";
        route.route_reason = "forced_cpu_a5";
        return route;
    }
    if (driver_candidates.empty()) {
        route.route_decision = "gpu_off";
        route.route_reason = "no_stage_a_safe_driver";
        return route;
    }
    if (gpu_backend == nullptr || !gpu_backend->is_available()) {
        route.route_decision = "gpu_off";
        route.route_reason = "gpu_backend_unavailable";
        return route;
    }

    if (mode == CompiledStageAMode::single || mode == CompiledStageAMode::multi) {
        route.chosen_drivers = choose_static_stage_a_drivers(driver_candidates, mode);
        route.best_single_drivers = route.chosen_drivers.empty()
                                        ? std::vector<std::string>{}
                                        : std::vector<std::string>{route.chosen_drivers.front()};
        if (mode == CompiledStageAMode::multi) {
            route.best_multi_drivers = route.chosen_drivers;
        }
        route.route_decision = route.chosen_drivers.size() > 1U ? "multi_driver" : "single_driver";
        route.route_reason = mode == CompiledStageAMode::multi ? "forced_multi_static" : "forced_single_static";
        route.gpu_enabled = !route.chosen_drivers.empty();
        if (route.gpu_enabled) {
            std::unordered_set<std::string> selected_ids(route.chosen_drivers.begin(), route.chosen_drivers.end());
            route.active_gpu_plan = build_gpu_prefilter_plan(runtime, &selected_ids);
            if (!gpu_prefilter_plan_fits_const_region_buffer(route.active_gpu_plan)) {
                route.route_decision = "gpu_off";
                route.route_reason = "gpu_region_count_exceeds_limit";
                route.gpu_enabled = false;
                route.active_gpu_plan = GpuPrefilterPlan{};
                route.multi_gpu_plan = GpuPrefilterPlan{};
                return route;
            }
            if (mode == CompiledStageAMode::multi) {
                route.multi_gpu_plan = route.active_gpu_plan;
            }
        }
        return route;
    }

    const uint64_t sample_count = std::min<uint64_t>(total_count, 65'536ULL);
    const bool measured_selection = sample_count >= 512ULL;

    if (!measured_selection) {
        route.chosen_drivers = choose_static_stage_a_drivers(driver_candidates, mode);
        route.route_decision = route.chosen_drivers.size() > 1U ? "multi_driver" : "single_driver";
        route.route_reason = "fallback_small_warmup";
        route.gpu_enabled = !route.chosen_drivers.empty();
        if (route.gpu_enabled) {
            std::unordered_set<std::string> selected_ids(route.chosen_drivers.begin(), route.chosen_drivers.end());
            route.active_gpu_plan = build_gpu_prefilter_plan(runtime, &selected_ids);
            if (!gpu_prefilter_plan_fits_const_region_buffer(route.active_gpu_plan)) {
                route.route_decision = "gpu_off";
                route.route_reason = "gpu_region_count_exceeds_limit";
                route.gpu_enabled = false;
                route.active_gpu_plan = GpuPrefilterPlan{};
                return route;
            }
        }
        return route;
    }

    std::vector<StageADriverMeasurement> single_measurements;
    single_measurements.reserve(driver_candidates.size());
    for (const std::string &driver : driver_candidates) {
        single_measurements.push_back(
            measure_stage_a_driver_combo(gpu_backend[0], start_seed, sample_count, runtime, {driver}));
    }
    std::stable_sort(single_measurements.begin(), single_measurements.end(), [](const StageADriverMeasurement &lhs,
                                                                                const StageADriverMeasurement &rhs) {
        return stage_a_measurement_better(lhs, rhs);
    });
    if (single_measurements.empty()) {
        route.route_decision = "gpu_off";
        route.route_reason = "no_measured_driver";
        return route;
    }

    const StageADriverMeasurement best_single = single_measurements.front();
    if (!best_single.measured) {
        route.route_decision = "gpu_off";
        route.route_reason = "gpu_region_count_exceeds_limit";
        return route;
    }
    route.best_single_drivers = best_single.drivers;
    route.measured_single_driver_pass_rate = best_single.pass_rate;

    StageADriverMeasurement best_selected = best_single;
    StageADriverMeasurement best_multi{};
    bool have_best_multi = false;

    const bool allow_pair_search =
        mode == CompiledStageAMode::multi || mode == CompiledStageAMode::auto_mode;
    if (allow_pair_search && driver_candidates.size() > 1U) {
        const size_t pool_size = std::min<size_t>(4U, single_measurements.size());
        for (size_t i = 0; i < pool_size; ++i) {
            for (size_t j = i + 1U; j < pool_size; ++j) {
                StageADriverMeasurement combo = measure_stage_a_driver_combo(
                    gpu_backend[0],
                    start_seed,
                    sample_count,
                    runtime,
                    {single_measurements[i].drivers.front(), single_measurements[j].drivers.front()});
                if (!have_best_multi || stage_a_measurement_better(combo, best_multi)) {
                    best_multi = combo;
                    have_best_multi = true;
                }
                if (stage_a_measurement_better(combo, best_selected)) {
                    best_selected = combo;
                }
            }
        }
    }

    const bool allow_triple_search =
        driver_candidates.size() > 2U &&
        (mode == CompiledStageAMode::multi || mode == CompiledStageAMode::auto_mode);
    if (allow_triple_search) {
        const size_t triple_pool = std::min<size_t>(4U, single_measurements.size());
        if (triple_pool >= 3U) {
            for (size_t i = 0; i < triple_pool; ++i) {
                for (size_t j = i + 1U; j < triple_pool; ++j) {
                    for (size_t k = j + 1U; k < triple_pool; ++k) {
                        StageADriverMeasurement combo = measure_stage_a_driver_combo(
                            gpu_backend[0],
                            start_seed,
                            sample_count,
                            runtime,
                            {single_measurements[i].drivers.front(),
                             single_measurements[j].drivers.front(),
                             single_measurements[k].drivers.front()});
                        if (!have_best_multi || stage_a_measurement_better(combo, best_multi)) {
                            best_multi = combo;
                            have_best_multi = true;
                        }
                        if (stage_a_measurement_better(combo, best_selected)) {
                            best_selected = combo;
                        }
                    }
                }
            }
        }
    }

    if ((mode == CompiledStageAMode::multi || mode == CompiledStageAMode::auto_mode) &&
        driver_candidates.size() > 3U) {
        StageADriverMeasurement combo =
            measure_stage_a_driver_combo(gpu_backend[0], start_seed, sample_count, runtime, driver_candidates);
        if (!have_best_multi || stage_a_measurement_better(combo, best_multi)) {
            best_multi = combo;
            have_best_multi = true;
        }
        if (stage_a_measurement_better(combo, best_selected)) {
            best_selected = combo;
        }
    }

    route.best_multi_drivers = have_best_multi ? best_multi.drivers : std::vector<std::string>{};
    route.measured_multi_driver_pass_rate = have_best_multi ? best_multi.pass_rate : -1.0;
    route.chosen_drivers = best_selected.drivers;
    route.measured_selected_pass_rate = best_selected.pass_rate;
    route.route_decision = route.chosen_drivers.size() > 1U ? "multi_driver" : "single_driver";
    route.route_reason = route.chosen_drivers.size() > 1U ? "measured_best_multi" : "measured_best_single";
    route.gpu_enabled = !route.chosen_drivers.empty();
    if (route.gpu_enabled) {
        std::unordered_set<std::string> selected_ids(route.chosen_drivers.begin(), route.chosen_drivers.end());
        route.active_gpu_plan = build_gpu_prefilter_plan(runtime, &selected_ids);
        if (!gpu_prefilter_plan_fits_const_region_buffer(route.active_gpu_plan)) {
            route.route_decision = "gpu_off";
            route.route_reason = "gpu_region_count_exceeds_limit";
            route.gpu_enabled = false;
            route.active_gpu_plan = GpuPrefilterPlan{};
            route.multi_gpu_plan = GpuPrefilterPlan{};
            return route;
        }
    }
    if (!route.best_multi_drivers.empty()) {
        std::unordered_set<std::string> multi_ids(route.best_multi_drivers.begin(), route.best_multi_drivers.end());
        route.multi_gpu_plan = build_gpu_prefilter_plan(runtime, &multi_ids);
        if (!gpu_prefilter_plan_fits_const_region_buffer(route.multi_gpu_plan)) {
            route.multi_gpu_plan = GpuPrefilterPlan{};
            route.best_multi_drivers.clear();
            route.measured_multi_driver_pass_rate = -1.0;
        }
    }
    return route;
}

static PlanDiagnosticsSummary build_plan_diagnostics_summary(
    const QueryRuntime &runtime,
    const CompiledQueryPlanContext &query_plan,
    const std::unordered_map<std::string, int> &constraint_index,
    CompiledStageAMode compiled_stage_a_mode
) {
    PlanDiagnosticsSummary summary{};
    summary.completeness_policy = runtime.spec.safety.completeness;
    summary.compiled_stage_a_mode = compiled_stage_a_mode_name(compiled_stage_a_mode);
    summary.gpu_stage_a_enabled = compiled_stage_a_mode_uses_gpu(compiled_stage_a_mode);
    summary.route_decision = summary.gpu_stage_a_enabled ? "gpu" : "cpu-only";
    switch (compiled_stage_a_mode) {
    case CompiledStageAMode::single:
        summary.route_reason = "single_driver";
        break;
    case CompiledStageAMode::multi:
        summary.route_reason = "multi_driver";
        break;
    case CompiledStageAMode::cpu_a5:
        summary.route_reason = "cpu_stage_a5_heavy";
        break;
    case CompiledStageAMode::gpu_off:
        summary.route_reason = "gpu_off";
        break;
    case CompiledStageAMode::auto_mode:
    default:
        summary.route_reason = "auto";
        break;
    }
    summary.compiled_stats_available =
        query_plan.compiled_ready() && scanner_core_query_plan_api().has_compiled_batch_stats_api();

    const std::vector<std::string> driver_candidates =
        ordered_stage_a_driver_candidates(runtime, query_plan, constraint_index);
    const size_t driver_limit = compiled_stage_a_mode == CompiledStageAMode::multi
                                    ? std::min<size_t>(2U, driver_candidates.size())
                                    : std::min<size_t>(1U, driver_candidates.size());
    for (size_t i = 0; i < driver_limit; ++i) {
        summary.chosen_drivers.push_back(driver_candidates[i]);
    }

    double score = 0.0;
    for (const std::string &cid : runtime.ordered_constraint_ids) {
        const auto it_idx = constraint_index.find(cid);
        const auto it_rt = runtime.constraints.find(cid);
        if (it_idx == constraint_index.end() || it_rt == runtime.constraints.end()) {
            continue;
        }
        const NativeQueryConstraintDesc &desc = query_plan.constraints[static_cast<size_t>(it_idx->second)];
        std::string label = "stage_b_only";
        if (is_stage_a_independent_candidate(desc)) {
            label = "stage_a_independent";
            summary.groups.independent.push_back(cid);
            summary.target_caps.push_back(cid + "=" + fmt_u64(std::max<uint32_t>(1U, desc.min_required)));
            score += 1000.0 / static_cast<double>(std::max<uint32_t>(1U, desc.candidate_cap));
        } else if (is_stage_a_dependent_candidate(desc)) {
            label = "stage_a_dependent";
            summary.groups.dependent.push_back(cid);
            summary.target_caps.push_back(cid + "=" + fmt_u64(std::max<uint32_t>(1U, desc.min_required)));
            score += 300.0 / static_cast<double>(std::max<uint32_t>(1U, desc.candidate_cap));
        } else if (is_stage_a_reject_only_candidate(desc)) {
            label = "stage_a_reject_only";
            summary.groups.reject_only.push_back(cid);
            summary.target_caps.push_back(cid + "=" + fmt_u64(std::max<uint32_t>(1U, desc.min_required)));
            score += 100.0 / static_cast<double>(std::max<uint32_t>(1U, desc.candidate_cap));
        } else {
            summary.groups.stage_b_only.push_back(cid);
            summary.target_caps.push_back(cid + "=" + fmt_u64(std::max<uint32_t>(1U, desc.min_required)));
            score += 10.0;
        }
        (void)label;
    }
    summary.predicted_score = score;
    return summary;
}

static DetailSnapshot collect_detail_snapshot_legacy(
    const CompiledQueryPlanContext &query_plan,
    uint64_t seed,
    bool use_biome_filters,
    uint32_t match_cap,
    uint32_t biome_cap
) {
    DetailSnapshot snapshot{};
    snapshot.matches.resize(match_cap);
    snapshot.biomes.resize(biome_cap);
    while (true) {
        uint32_t match_count = 0U;
        uint32_t biome_count = 0U;
        snapshot.cap_pruned = 0U;
        const int rc = query_plan.collect_details_legacy(
            seed,
            snapshot.matches.empty() ? nullptr : snapshot.matches.data(),
            static_cast<uint32_t>(snapshot.matches.size()),
            &match_count,
            use_biome_filters && !snapshot.biomes.empty() ? snapshot.biomes.data() : nullptr,
            use_biome_filters ? static_cast<uint32_t>(snapshot.biomes.size()) : 0U,
            &biome_count,
            &snapshot.cap_pruned,
            use_biome_filters
        );
        snapshot.rc = rc;
        if (rc != 0) {
            snapshot.matches.clear();
            snapshot.biomes.clear();
            return snapshot;
        }
        if (match_count <= snapshot.matches.size() && biome_count <= snapshot.biomes.size()) {
            snapshot.matches.resize(match_count);
            snapshot.biomes.resize(biome_count);
            return snapshot;
        }
        snapshot.matches.resize(match_count);
        snapshot.biomes.resize(biome_count);
    }
}

static DetailSnapshot collect_detail_snapshot_compiled(
    const CompiledQueryPlanContext &query_plan,
    uint64_t seed,
    bool use_biome_filters,
    uint32_t match_cap,
    uint32_t biome_cap
) {
    (void)use_biome_filters;
    DetailSnapshot snapshot{};
    snapshot.matches.resize(match_cap);
    snapshot.biomes.resize(biome_cap);
    while (true) {
        uint32_t match_count = 0U;
        uint32_t biome_count = 0U;
        snapshot.cap_pruned = 0U;
        const int rc = query_plan.collect_details_compiled(
            seed,
            snapshot.matches.empty() ? nullptr : snapshot.matches.data(),
            static_cast<uint32_t>(snapshot.matches.size()),
            &match_count,
            snapshot.biomes.empty() ? nullptr : snapshot.biomes.data(),
            static_cast<uint32_t>(snapshot.biomes.size()),
            &biome_count,
            &snapshot.cap_pruned
        );
        snapshot.rc = rc;
        if (rc != 0) {
            snapshot.matches.clear();
            snapshot.biomes.clear();
            return snapshot;
        }
        if (match_count <= snapshot.matches.size() && biome_count <= snapshot.biomes.size()) {
            snapshot.matches.resize(match_count);
            snapshot.biomes.resize(biome_count);
            return snapshot;
        }
        snapshot.matches.resize(match_count);
        snapshot.biomes.resize(biome_count);
    }
}

static bool snapshots_equal(const DetailSnapshot &lhs, const DetailSnapshot &rhs) {
    if (lhs.rc != rhs.rc || lhs.cap_pruned != rhs.cap_pruned) {
        return false;
    }
    if (lhs.matches.size() != rhs.matches.size() || lhs.biomes.size() != rhs.biomes.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.matches.size(); ++i) {
        const NativeQueryMatchOut &a = lhs.matches[i];
        const NativeQueryMatchOut &b = rhs.matches[i];
        if (a.constraint_index != b.constraint_index || a.x != b.x || a.z != b.z || a.anchor_x != b.anchor_x ||
            a.anchor_z != b.anchor_z || a.dist2 != b.dist2) {
            return false;
        }
    }
    for (size_t i = 0; i < lhs.biomes.size(); ++i) {
        const NativeQueryBiomeOut &a = lhs.biomes[i];
        const NativeQueryBiomeOut &b = rhs.biomes[i];
        if (a.filter_index != b.filter_index || a.x != b.x || a.y != b.y || a.z != b.z || a.radius != b.radius ||
            a.dist2 != b.dist2 || a.biome_id != b.biome_id) {
            return false;
        }
    }
    return true;
}

static std::string describe_plan_diagnostics_json(
    const PlanDiagnosticsSummary &summary,
    const ScannerCompiledBatchStats *stats,
    const RunBatchStats *batch_stats
) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"completeness\":\"" << json_escape(summary.completeness_policy) << "\",";
    oss << "\"compiled_stage_a_mode\":\"" << json_escape(summary.compiled_stage_a_mode) << "\",";
    oss << "\"route_decision\":\"" << json_escape(summary.route_decision) << "\"";
    oss << ",\"route_reason\":\"" << json_escape(summary.route_reason) << "\"";
    oss << ",\"gpu_stage_a_enabled\":" << (summary.gpu_stage_a_enabled ? "true" : "false");
    oss << ",\"chosen_driver\":[";
    for (size_t i = 0; i < summary.chosen_drivers.size(); ++i) {
        if (i != 0U) {
            oss << ",";
        }
        oss << "\"" << json_escape(summary.chosen_drivers[i]) << "\"";
    }
    oss << "]";
    oss << ",\"stage_a_independent\":[";
    for (size_t i = 0; i < summary.groups.independent.size(); ++i) {
        if (i != 0U) {
            oss << ",";
        }
        oss << "\"" << json_escape(summary.groups.independent[i]) << "\"";
    }
    oss << "]";
    oss << ",\"stage_a_dependent\":[";
    for (size_t i = 0; i < summary.groups.dependent.size(); ++i) {
        if (i != 0U) {
            oss << ",";
        }
        oss << "\"" << json_escape(summary.groups.dependent[i]) << "\"";
    }
    oss << "]";
    oss << ",\"stage_a_reject_only\":[";
    for (size_t i = 0; i < summary.groups.reject_only.size(); ++i) {
        if (i != 0U) {
            oss << ",";
        }
        oss << "\"" << json_escape(summary.groups.reject_only[i]) << "\"";
    }
    oss << "]";
    oss << ",\"stage_b_only\":[";
    for (size_t i = 0; i < summary.groups.stage_b_only.size(); ++i) {
        if (i != 0U) {
            oss << ",";
        }
        oss << "\"" << json_escape(summary.groups.stage_b_only[i]) << "\"";
    }
    oss << "]";
    oss << ",\"target_caps\":[";
    for (size_t i = 0; i < summary.target_caps.size(); ++i) {
        if (i != 0U) {
            oss << ",";
        }
        oss << "\"" << json_escape(summary.target_caps[i]) << "\"";
    }
    oss << "]";
    oss << ",\"predicted_plan_score\":" << std::fixed << std::setprecision(3) << summary.predicted_score;
    oss << ",\"measured_single_driver_pass_rate\":" << std::fixed << std::setprecision(6)
        << summary.measured_single_driver_pass_rate;
    oss << ",\"measured_selected_pass_rate\":" << std::fixed << std::setprecision(6)
        << summary.measured_selected_pass_rate;
    oss << ",\"compiled_stats_available\":" << (summary.compiled_stats_available ? "true" : "false");
    oss << ",\"observed_stage_a_pass_rate\":" << std::fixed << std::setprecision(6)
        << summary.observed_stage_a_pass_rate;
    oss << ",\"observed_stage_b_reject_rate\":" << std::fixed << std::setprecision(6)
        << summary.observed_stage_b_reject_rate;
    if (batch_stats != nullptr) {
        oss << ",\"batch_input_seed_count\":" << batch_stats->input_seed_count;
        oss << ",\"batch_stage_a_survivor_count\":" << batch_stats->stage_a_survivor_count;
        oss << ",\"batch_stage_a5_survivor_count\":" << batch_stats->stage_a5_survivor_count;
        oss << ",\"batch_stage_b_structure_survivor_count\":" << batch_stats->stage_b_structure_survivor_count;
        oss << ",\"batch_stage_c_biome_survivor_count\":" << batch_stats->stage_c_biome_survivor_count;
        oss << ",\"batch_final_valid_count\":" << batch_stats->final_valid_count;
        oss << ",\"batch_stage_a_reject_count\":" << batch_stats->stage_a_reject_count;
        oss << ",\"batch_stage_a5_reject_count\":" << batch_stats->stage_a5_reject_count;
        oss << ",\"batch_stage_b_exact_reject_count\":" << batch_stats->stage_b_exact_reject_count;
        oss << ",\"batch_stage_c_biome_reject_count\":" << batch_stats->stage_c_biome_reject_count;
        oss << ",\"batch_mismatches\":" << batch_stats->mismatches;
        oss << ",\"batch_biome_rejected\":" << batch_stats->biome_rejected;
        oss << ",\"batch_sculpt_rejected\":" << batch_stats->sculpt_rejected;
        oss << ",\"batch_loot_rejected\":" << batch_stats->loot_rejected;
        oss << ",\"batch_cap_pruned_total\":" << batch_stats->cap_pruned_total;
        oss << ",\"batch_gpu_kernel_seconds\":" << std::fixed << std::setprecision(6)
            << batch_stats->gpu_kernel_seconds;
        oss << ",\"batch_gpu_transfer_compaction_seconds\":" << std::fixed << std::setprecision(6)
            << batch_stats->gpu_transfer_compaction_seconds;
        oss << ",\"batch_cpu_stage_a5_seconds\":" << std::fixed << std::setprecision(6)
            << batch_stats->cpu_stage_a5_seconds;
        oss << ",\"batch_total_seconds\":" << std::fixed << std::setprecision(6) << batch_stats->total_seconds;
        oss << ",\"batch_gpu_seconds\":" << std::fixed << std::setprecision(6) << batch_stats->gpu_seconds;
        oss << ",\"batch_cpu_stage_b_seconds\":" << std::fixed << std::setprecision(6)
            << batch_stats->cpu_stage_b_seconds;
        oss << ",\"batch_cpu_stage_c_seconds\":" << std::fixed << std::setprecision(6)
            << batch_stats->cpu_stage_c_seconds;
    }
    if (stats != nullptr) {
        oss << ",\"cubiomes_setup_generator_calls\":" << stats->setup_generator_calls;
        oss << ",\"cubiomes_apply_seed_calls\":" << stats->apply_seed_calls;
        oss << ",\"cubiomes_get_structure_pos_calls\":" << stats->get_structure_pos_calls;
        oss << ",\"cubiomes_is_viable_structure_pos_calls\":" << stats->is_viable_structure_pos_calls;
        oss << ",\"cubiomes_get_biome_at_calls\":" << stats->get_biome_at_calls;
        oss << ",\"cubiomes_get_spawn_calls\":" << stats->get_spawn_calls;
        oss << ",\"cubiomes_estimate_spawn_calls\":" << stats->estimate_spawn_calls;
        oss << ",\"cubiomes_init_first_stronghold_calls\":" << stats->init_first_stronghold_calls;
        oss << ",\"cubiomes_next_stronghold_calls\":" << stats->next_stronghold_calls;
        if (stats->constraint_count > 0U) {
            oss << ",\"per_constraint_stage_a_rejects\":[";
            for (uint32_t i = 0; i < stats->constraint_count; ++i) {
                if (i != 0U) {
                    oss << ",";
                }
                oss << (stats->per_constraint_stage_a_rejects != nullptr
                            ? stats->per_constraint_stage_a_rejects[i]
                            : 0ULL);
            }
            oss << "]";
            oss << ",\"per_constraint_stage_a5_rejects\":[";
            for (uint32_t i = 0; i < stats->constraint_count; ++i) {
                if (i != 0U) {
                    oss << ",";
                }
                oss << (stats->per_constraint_stage_a5_rejects != nullptr
                            ? stats->per_constraint_stage_a5_rejects[i]
                            : 0ULL);
            }
            oss << "]";
            oss << ",\"per_constraint_stage_b_exact_rejects\":[";
            for (uint32_t i = 0; i < stats->constraint_count; ++i) {
                if (i != 0U) {
                    oss << ",";
                }
                oss << (stats->per_constraint_stage_b_exact_rejects != nullptr
                            ? stats->per_constraint_stage_b_exact_rejects[i]
                            : 0ULL);
            }
            oss << "]";
        }
    }
    oss << "}";
    return oss.str();
}

static bool query_uses_dependent_candidate_metadata(const QuerySpec &spec) {
    for (const ConstraintSpec &constraint : spec.constraints) {
        if (constraint.anchor.rfind("constraint:", 0) == 0) {
            return true;
        }
    }
    for (const BiomeFilterSpec &filter : spec.biome_filters) {
        if (filter.point.rfind("constraint:", 0) == 0) {
            return true;
        }
    }
    return !spec.loot_filters.empty();
}

static bool query_has_full64_required_predicates(const QuerySpec &spec) {
    if (!spec.biome_filters.empty() || spec.sculpt.enabled || !spec.loot_filters.empty()) {
        return true;
    }
    for (const ConstraintSpec &constraint : spec.constraints) {
        if (constraint.mode == "strict" || constraint.anchor == "spawn") {
            return true;
        }
    }
    return false;
}

static bool query_has_spawn_anchor(const QuerySpec &spec) {
    for (const ConstraintSpec &constraint : spec.constraints) {
        if (constraint.anchor == "spawn") {
            return true;
        }
    }
    for (const BiomeFilterSpec &filter : spec.biome_filters) {
        if (filter.point == "spawn") {
            return true;
        }
    }
    return spec.sculpt.enabled && spec.sculpt.center == "spawn";
}

static bool query_has_quad_constraint(const QuerySpec &spec) {
    for (const ConstraintSpec &constraint : spec.constraints) {
        if (constraint.min_required > 1U) {
            return true;
        }
    }
    return false;
}

static bool query_has_structure_settings(const QuerySpec &spec) {
    for (const ConstraintSpec &constraint : spec.constraints) {
        if (!constraint.structure_settings.empty()) {
            return true;
        }
    }
    return false;
}

static bool query_has_strict_constraint(const QuerySpec &spec) {
    for (const ConstraintSpec &constraint : spec.constraints) {
        if (constraint.mode == "strict") {
            return true;
        }
    }
    return false;
}

static bool query_has_strict_java_validated_constraint(const QuerySpec &spec) {
    for (const ConstraintSpec &constraint : spec.constraints) {
        if (constraint.mode == "strict" && constraint.structure != "stronghold") {
            return true;
        }
    }
    return false;
}

static void enforce_exact_completeness_policy(const QuerySpec &spec, const CliArgs &args) {
    if (!is_exact_policy(spec)) {
        return;
    }

    std::string surrogate = lower_ascii(trim(spec.perf.strict_surrogate));
    if (surrogate == "light-speed") {
        surrogate = "lightspeed";
    }
    if (surrogate.empty()) {
        surrogate = "off";
    }
    if (surrogate != "off") {
        throw std::runtime_error(
            "Exact scans require performance.strict_surrogate=off because surrogate gates and coarse biome samples "
            "can skip valid seeds. Use safety.completeness=approximate for those speed shortcuts."
        );
    }

    if (args.scan_mode == "placement") {
        throw std::runtime_error(
            "Exact scans cannot use --scan-mode placement. Placement mode reports raw structure-set attempts and does "
            "not prove vanilla post-placement generation."
        );
    }

    if (!spec.biome_filters.empty()) {
        throw std::runtime_error(
            "Exact biome filters are disabled because native biome filters currently use an approximate fixed-Y "
            "sampling shape. Use safety.completeness=approximate until the vanilla quart sampler is enabled."
        );
    }

    if (query_has_spawn_anchor(spec)) {
        throw std::runtime_error(
            "Exact spawn anchors are disabled because native spawn resolution uses the cubiomes climate spawn, not the "
            "final Minecraft server spawn adjustment. Use safety.completeness=approximate for spawn-anchored scans."
        );
    }

    if (!spec.loot_filters.empty()) {
        throw std::runtime_error(
            "Exact loot filters are disabled for 26.1.2 because the current replay does not yet derive loot from the "
            "vanilla selected structure start and actual placed chests. Use safety.completeness=approximate."
        );
    }

    if (query_has_structure_settings(spec)) {
        throw std::runtime_error(
            "Exact structure_settings are disabled because village building checks can fall back to approximate "
            "jigsaw simulation. Use safety.completeness=approximate."
        );
    }

    if (args.java_cubiomes_validation == "off" && query_has_strict_java_validated_constraint(spec)) {
        throw std::runtime_error(
            "Exact strict structure scans require Java vanilla structure validation. Use --java-cubiomes-validation "
            "auto or strict, or set safety.completeness=approximate."
        );
    }

    if (query_uses_dependent_candidate_metadata(spec) && spec.perf.candidate_cap_mode != "none") {
        throw std::runtime_error(
            "Exact scans with dependent anchors, dependent biome filters, or loot filters require "
            "performance.candidate_cap_mode=none so anchor candidates are not truncated."
        );
    }

    if (query_has_quad_constraint(spec) && spec.perf.candidate_cap_mode != "none") {
        throw std::runtime_error(
            "Exact multi-match/quad constraints require performance.candidate_cap_mode=none so compact groups are not "
            "tested against a truncated candidate list."
        );
    }

    if (args.java_cubiomes_validation != "off" && query_has_strict_constraint(spec) &&
        spec.perf.candidate_cap_mode != "none") {
        throw std::runtime_error(
            "Exact Java structure validation requires performance.candidate_cap_mode=none so later vanilla-valid "
            "structure starts are not skipped by candidate caps."
        );
    }

    if (args.scan_mode == "mixed" && args.mixed_survivor_cap != 0U) {
        throw std::runtime_error(
            "Exact mixed scans require --mixed-survivor-cap 0. Survivor caps are approximate because they can drop "
            "placement-valid seeds before strict validation."
        );
    }

    if (args.output_mode != "raw" && args.upper16 != 0U && query_has_full64_required_predicates(spec)) {
        throw std::runtime_error(
            "Exact full64 output is not implemented for strict/biome/spawn/loot predicates yet. The scanner validates "
            "the lower48 seed stream, so use raw output or upper16=0 until upper16 enumeration is added."
        );
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);

        const CliArgs args = parse_cli(argc, argv);
        g_debug_enabled = args.debug_enabled;
        if (args.list_structures) {
            print_supported_structures();
            return 0;
        }
        if (args.dump_query_template) {
            print_query_template();
            return 0;
        }

        const fs::path repo_root = detect_repo_root(argv[0]);
        const auto biome_map = build_biome_map(repo_root);

        std::string query_text;
        if (!args.query_file.empty() && !args.query_json.empty()) {
            throw std::runtime_error("Use either --query-file or --query-json, not both.");
        }
        if (!args.query_file.empty()) {
            query_text = read_text_file_utf8_bom_tolerant(args.query_file);
        } else if (!args.query_json.empty()) {
            query_text = args.query_json;
        } else {
            throw std::runtime_error("Native scanner requires --query-json or --query-file.");
        }

        const JsonValue raw_query = JsonParser(query_text).parse();
        QuerySpec query_spec = parse_query_spec(raw_query, biome_map);
        if (!args.execution_mode.empty()) {
            query_spec.perf.execution_mode = args.execution_mode;
        }
        if (!args.completeness_policy.empty()) {
            query_spec.safety.completeness = args.completeness_policy;
        }
        enforce_exact_completeness_policy(query_spec, args);
        std::string requested_compiled_stage_a_mode = args.compiled_stage_a_mode;
        if (!args.compiled_stage_a_mode_set) {
            requested_compiled_stage_a_mode = query_spec.perf.compiled_stage_a_mode;
        }
        const ExecutionModePlan exec_mode_plan = resolve_execution_mode_plan(query_spec.perf.execution_mode);
        const CompiledStageAMode compiled_stage_a_mode = parse_compiled_stage_a_mode(requested_compiled_stage_a_mode);
        const QueryRuntime runtime = build_query_runtime(query_spec);
        auto [native_regions, native_constraints, constraint_index] = build_native_query_arrays(runtime);
        auto [native_biome_filters, native_biome_allowed] = build_native_query_biome_arrays(query_spec, constraint_index);
        auto [native_sculpt, native_sculpt_allowed] = build_native_sculpt_config(query_spec);
        const int cubi_mc_version = resolve_mc_version(args.mc_version);
        const uint64_t start_seed = (args.has_start ? args.start : choose_default_start()) & JAVA_MASK;
        if (args.count == 0) {
            throw std::runtime_error("--count must be > 0.");
        }
        CompiledQueryPlanContext query_plan;
        query_plan.build(
            std::move(native_regions),
            std::move(native_constraints),
            std::move(native_biome_filters),
            std::move(native_biome_allowed),
            cubi_mc_version
        );
        if (args.shadow_compare && !query_plan.compiled_ready()) {
            throw std::runtime_error("shadow-compare requires compiled query plan support.");
        }
        const std::string loot_version_token = trim(args.mc_version);
        const std::vector<PreparedLootFilter> prepared_loot_filters =
            prepare_loot_filters(runtime, constraint_index, loot_version_token);
        const std::vector<PreparedStructureSettingsFilter> prepared_structure_settings_filters =
            prepare_structure_settings_filters(runtime, constraint_index);
        const std::vector<PreparedJavaStructureValidationConstraint> prepared_java_structure_filters =
            prepare_java_structure_validation_constraints(runtime, constraint_index);
        const bool loot_enabled = !prepared_loot_filters.empty();
        const bool structure_settings_enabled = !prepared_structure_settings_filters.empty();
        const bool java_cubiomes_validation_enabled =
            !prepared_java_structure_filters.empty() &&
            (args.java_cubiomes_validation == "strict" ||
             (args.java_cubiomes_validation == "auto" &&
              is_exact_policy(query_spec) &&
              is_exact_java_cubiomes_validation_version(loot_version_token)));
        const bool sculpt_enabled = native_sculpt.enabled != 0U;

        GpuFilterApi gpu_backend;
        const bool gpu_backend_loaded = gpu_backend.load(repo_root, args.gpu_backend) && gpu_backend.is_available();
        if ((compiled_stage_a_mode == CompiledStageAMode::single ||
             compiled_stage_a_mode == CompiledStageAMode::multi) &&
            !gpu_backend_loaded) {
            const std::string &requested_backend =
                args.gpu_backend == "auto" ? std::string("cuda") : args.gpu_backend;
            const char *missing_fname = gpu_filter_library_filename_for(requested_backend);
            const char *missing_readme = gpu_backend_readme_path_for(requested_backend);
            const char *missing_env = requested_backend == "opencl" ? "GPU_FILTER_OPENCL_DLL" : "GPU_FILTER_DLL";
            std::string hint;
            if (args.gpu_backend == "auto") {
                hint = " (auto-selection tried CUDA then OpenCL and neither was found; pick one explicitly with --gpu-backend)";
            } else {
                hint = " (requested via --gpu-backend " + args.gpu_backend + ")";
            }
            throw std::runtime_error(
                std::string("Native GPU backend not available. Build ") + missing_fname +
                " (see " + missing_readme + ") or set " + missing_env + " to the built library path." + hint
            );
        }
        CompiledStageAMode route_stage_a_mode = compiled_stage_a_mode;
        StageARoutePlan stage_a_route = choose_stage_a_route_plan(
            runtime,
            query_plan,
            constraint_index,
            route_stage_a_mode,
            gpu_backend_loaded ? &gpu_backend : nullptr,
            start_seed,
            args.count
        );
        PlanDiagnosticsSummary plan_summary =
            build_plan_diagnostics_summary(runtime, query_plan, constraint_index, route_stage_a_mode);
        plan_summary.chosen_drivers = stage_a_route.chosen_drivers;
        plan_summary.route_decision = stage_a_route.route_decision;
        plan_summary.route_reason = stage_a_route.route_reason;
        plan_summary.gpu_stage_a_enabled = stage_a_route.gpu_enabled;
        plan_summary.measured_single_driver_pass_rate = stage_a_route.measured_single_driver_pass_rate;
        plan_summary.measured_selected_pass_rate = stage_a_route.measured_selected_pass_rate;
        GpuPrefilterPlan gpu_prefilter_plan = stage_a_route.active_gpu_plan;
        bool use_gpu_stage_a = stage_a_route.gpu_enabled;
        if ((args.scan_mode == "placement" || args.scan_mode == "mixed") &&
            (!use_gpu_stage_a || gpu_prefilter_plan.constraints.empty())) {
            const std::string &requested_backend =
                args.gpu_backend == "auto" ? std::string("cuda") : args.gpu_backend;
            throw std::runtime_error(
                "--scan-mode " + args.scan_mode +
                " requires an active GPU placement prefilter. Use a placement-supported structure query and build " +
                gpu_filter_library_filename_for(requested_backend) + "."
            );
        }
        const bool gpu_stage_a_final_filter = gpu_stage_a_is_final_placement_filter(
            runtime,
            query_plan,
            stage_a_route,
            sculpt_enabled,
            loot_enabled,
            structure_settings_enabled
        );

        const bool needs_cubiomes = std::any_of(
                                        query_plan.constraints.begin(),
                                        query_plan.constraints.end(),
                                        [](const NativeQueryConstraintDesc &c) {
                                            return c.mode_strict != 0U || c.is_stronghold != 0U ||
                                                   c.anchor_type == SCAN_ANCHOR_SPAWN;
                                        }) ||
                                    !query_plan.biome_filters.empty() || sculpt_enabled || loot_enabled ||
                                    structure_settings_enabled ||
                                    java_cubiomes_validation_enabled;

        if (needs_cubiomes) {
            const fs::path cubi_dll = resolve_cubiomes_lib_path(repo_root);
            if (cubi_dll.empty()) {
                throw std::runtime_error(
                    std::string("Final validated results require cubiomes ") + cubiomes_library_filename() + ". "
                    +
                    "Set SCANNER_CUBIOMES_DLL or install cubiomespi."
                );
            }
            const int set_ok = scanner_core_set_cubiomes_lib_path(cubi_dll.string().c_str());
            if (set_ok != 1) {
                throw std::runtime_error("Failed to load cubiomes lib for scanner_core: " + cubi_dll.string());
            }
        }

        LootValidationServerClient loot_client;
        if (loot_enabled || structure_settings_enabled || java_cubiomes_validation_enabled) {
            std::string loot_error;
            if (!loot_client.start(repo_root, loot_error)) {
                throw std::runtime_error("Failed to start LootValidationServer: " + loot_error);
            }
        }

        std::atomic<bool> terminal_mode(args.terminal_mode);
        int strict_workers = args.strict_workers_set ? args.strict_workers : query_spec.perf.strict_workers;
        if (strict_workers <= 0) {
            unsigned hc = std::thread::hardware_concurrency();
            if (hc == 0U) {
                hc = 8U;
            }
            strict_workers = static_cast<int>(std::min<unsigned>(32U, hc));
        }
        strict_workers = std::max(1, strict_workers);
        if (!args.strict_workers_set && (loot_enabled || structure_settings_enabled)) {
            const int auto_loot_worker_cap = exec_mode_plan.requested == "max-throughput" ? 16 : 4;
            if (strict_workers > auto_loot_worker_cap) {
                // Exact loot replay is Java-heavy; keep auto conservative unless throughput mode asks for parallelism.
                strict_workers = auto_loot_worker_cap;
            }
        }
        const int requested_strict_workers = strict_workers;
        const uint64_t strict_gate_product = gpu_prefilter_gate_product(gpu_prefilter_plan);
        const bool strict_gate_active = strict_gate_product > 1ULL;
        if (args.scan_mode == "mixed" &&
            strict_gate_active &&
            (loot_enabled || structure_settings_enabled || java_cubiomes_validation_enabled)) {
            int sparse_worker_cap = strict_workers;
            if (strict_gate_product >= 16384ULL) {
                sparse_worker_cap = 1;
            } else if (strict_gate_product >= 4096ULL) {
                sparse_worker_cap = 2;
            } else if (strict_gate_product >= 1024ULL) {
                sparse_worker_cap = 4;
            }
            strict_workers = std::max(1, std::min(strict_workers, sparse_worker_cap));
        }

        std::vector<uint64_t> batch_probe_buffer;
        std::vector<uint64_t> batch_probe_valid;
        std::vector<uint64_t> batch_probe_sculpt;
        auto run_pipeline_probe = [&](uint64_t bs, bool probe_use_gpu, const GpuPrefilterPlan *probe_plan) {
            if (probe_use_gpu) {
                if (probe_plan == nullptr) {
                    throw std::runtime_error("GPU route probe missing prefilter plan.");
                }
                gpu_backend.filter_multi_into(
                    start_seed,
                    bs,
                    probe_plan->regions,
                    probe_plan->constraints,
                    batch_probe_buffer
                );
            } else {
                batch_probe_buffer.resize(static_cast<size_t>(bs));
                for (uint64_t i = 0; i < bs; ++i) {
                    batch_probe_buffer[static_cast<size_t>(i)] = (start_seed + i) & JAVA_MASK;
                }
            }
            if (batch_probe_buffer.empty()) {
                return;
            }
            if (batch_probe_valid.size() < batch_probe_buffer.size()) {
                batch_probe_valid.resize(batch_probe_buffer.size());
            }
            uint32_t valid_count = 0;
            uint32_t mismatch_count = 0;
            uint32_t biome_reject_count = 0;
            uint32_t cap_pruned = 0;
            const uint32_t probe_workers = choose_effective_validation_workers(
                static_cast<uint32_t>(strict_workers),
                batch_probe_buffer.size()
            );
            const int rc = query_plan.validate_batch(
                batch_probe_buffer.data(),
                static_cast<uint32_t>(batch_probe_buffer.size()),
                probe_workers,
                batch_probe_valid.data(),
                &valid_count,
                &mismatch_count,
                &biome_reject_count,
                &cap_pruned
            );
            if (rc != 0) {
                std::ostringstream oss;
                oss << "scanner_core_validate_query_batch failed with code " << fmt_i64(rc);
                throw std::runtime_error(oss.str());
            }
            if (sculpt_enabled && valid_count > 0U) {
                if (batch_probe_sculpt.size() < valid_count) {
                    batch_probe_sculpt.resize(valid_count);
                }
                uint32_t sculpt_valid_count = 0U;
                uint32_t sculpt_reject_count = 0U;
                const uint32_t sculpt_workers = choose_effective_validation_workers(
                    static_cast<uint32_t>(strict_workers),
                    valid_count
                );
                const int sculpt_rc = scanner_core_filter_sculpt_batch(
                    batch_probe_valid.data(),
                    valid_count,
                    &native_sculpt,
                    native_sculpt_allowed.data(),
                    static_cast<uint32_t>(native_sculpt_allowed.size()),
                    cubi_mc_version,
                    sculpt_workers,
                    batch_probe_sculpt.data(),
                    &sculpt_valid_count,
                    &sculpt_reject_count
                );
                if (sculpt_rc != 0) {
                    std::ostringstream oss;
                    oss << "scanner_core_filter_sculpt_batch failed with code " << fmt_i64(sculpt_rc);
                    throw std::runtime_error(oss.str());
                }
            }
        };

        const bool gpu_async_forced_off = args.gpu_pipeline == "sync";
        bool use_async_gpu_pipeline = false;
        if (use_gpu_stage_a && !gpu_prefilter_plan.constraints.empty() && !gpu_async_forced_off) {
            use_async_gpu_pipeline = gpu_backend.async_available();
        }
        if (
            args.gpu_pipeline == "async" &&
            !use_async_gpu_pipeline &&
            use_gpu_stage_a &&
            !gpu_prefilter_plan.constraints.empty()
        ) {
            std::ostringstream oss;
            oss << "--gpu-pipeline async requested, but ";
            const std::string &active = gpu_backend.active_backend().empty()
                ? std::string("cuda") : gpu_backend.active_backend();
            oss << gpu_filter_library_filename_for(active) << " does not expose the double-buffered async API";
            if (active == "opencl") {
                oss << " (the OpenCL backend is sync-only; use --gpu-backend cuda or --gpu-pipeline sync)";
            }
            oss << ".";
            throw std::runtime_error(oss.str());
        }
        const bool use_mixed_async_max_tuner =
            args.batch_size == "max" &&
            args.scan_mode == "mixed" &&
            use_gpu_stage_a &&
            use_async_gpu_pipeline;
        MixedAsyncTuningChoice mixed_async_tuning_choice{};
        uint64_t batch_size = 0ULL;
        if (use_mixed_async_max_tuner) {
            mixed_async_tuning_choice = choose_mixed_async_max_batch_and_depth(
                args,
                gpu_backend,
                gpu_prefilter_plan,
                start_seed,
                args.count,
                terminal_mode.load()
            );
            if (mixed_async_tuning_choice.valid) {
                batch_size = mixed_async_tuning_choice.batch_size;
            }
        }
        if (batch_size == 0ULL) {
            const uint64_t fallback_min_async_slots =
                (args.scan_mode == "mixed" && use_async_gpu_pipeline && args.gpu_inflight_batches != "auto")
                    ? std::max<uint64_t>(1ULL, std::min<uint64_t>(parse_u64(args.gpu_inflight_batches), 8ULL))
                    : 1ULL;
            batch_size = choose_batch_size(
                args,
                args.scan_mode == "mixed" ? false : needs_cubiomes,
                use_gpu_stage_a ? gpu_backend.total_mem_bytes() : 0ULL,
                args.count,
                [&](uint64_t bs) {
                    if (args.scan_mode == "mixed" && use_gpu_stage_a) {
                        if (use_async_gpu_pipeline) {
                            gpu_backend.submit_batch(
                                0U,
                                start_seed,
                                bs,
                                gpu_prefilter_plan.regions,
                                gpu_prefilter_plan.constraints
                            );
                            double kernel_seconds = 0.0;
                            double transfer_seconds = 0.0;
                            gpu_backend.collect_batch(0U, batch_probe_buffer, kernel_seconds, transfer_seconds);
                        } else {
                            gpu_backend.filter_multi_into(
                                start_seed,
                                bs,
                                gpu_prefilter_plan.regions,
                                gpu_prefilter_plan.constraints,
                                batch_probe_buffer
                            );
                        }
                        return;
                    }
                    run_pipeline_probe(bs, use_gpu_stage_a, use_gpu_stage_a ? &gpu_prefilter_plan : nullptr);
                },
                terminal_mode.load(),
                static_cast<uint32_t>(fallback_min_async_slots)
            );
        }
        uint64_t capped_batch_size = std::min<uint64_t>(batch_size, args.count);
        if (capped_batch_size == 0) {
            capped_batch_size = 1;
        }

        const uint32_t gpu_async_slots_available =
            use_async_gpu_pipeline ? std::max<uint32_t>(1U, gpu_backend.async_max_slots()) : 0U;
        const uint32_t gpu_inflight_batches = mixed_async_tuning_choice.valid
            ? mixed_async_tuning_choice.depth
            : choose_gpu_inflight_batches(
                  args,
                  gpu_backend,
                  gpu_prefilter_plan,
                  start_seed,
                  args.count,
                  capped_batch_size,
                  use_async_gpu_pipeline,
                  terminal_mode.load());
        if (args.plan_diagnostics || args.debug_enabled || terminal_mode.load()) {
            std::cout << "RUNTIME_DIAGNOSTICS {"
                      << "\"scanner_path\":\"" << json_escape(fs::absolute(fs::path(argv[0])).string()) << "\""
                      << ",\"repo_root\":\"" << json_escape(repo_root.string()) << "\""
                      << ",\"gpu_backend\":\""
                      << json_escape(
                             gpu_backend_loaded
                                 ? (gpu_backend.active_backend().empty() ? std::string("cuda") : gpu_backend.active_backend())
                                 : std::string("none"))
                      << "\""
                      << ",\"gpu_filter_path\":\""
                      << json_escape(gpu_backend_loaded ? gpu_backend.loaded_path().string() : std::string())
                      << "\""
                      << ",\"gpu_async_slots_available\":" << gpu_async_slots_available
                      << ",\"gpu_inflight_batches\":" << gpu_inflight_batches
                      << ",\"placement_batch_size\":" << capped_batch_size
                      << ",\"route_decision\":\"" << json_escape(stage_a_route.route_decision) << "\""
                      << ",\"route_reason\":\"" << json_escape(stage_a_route.route_reason) << "\""
                      << "}\n";
        }

        if (terminal_mode.load()) {
            std::cout << "Device: native-cpp\n";
            std::cout << "Native scanner: " << fs::absolute(fs::path(argv[0])).string() << "\n";
            std::cout << "Repo root: " << repo_root.string() << "\n";
            const uint64_t tail_count = args.count % JAVA_DOMAIN;
            const uint64_t range_end = (start_seed + tail_count) & JAVA_MASK;
            const uint64_t seeds_until_wrap = JAVA_DOMAIN - start_seed;
            const bool wraps_lower48 =
                args.count > seeds_until_wrap || (start_seed == 0ULL && args.count >= JAVA_DOMAIN);
            if (wraps_lower48) {
                std::cout << "Range: start=" << fmt_seed(start_seed)
                          << ", count=" << fmt_u64(args.count)
                          << ", end=" << fmt_seed(range_end)
                          << " lower48 (wraps";
                const uint64_t full_passes = args.count / JAVA_DOMAIN;
                if (full_passes > 0ULL) {
                    std::cout << ", full passes=" << fmt_u64(full_passes);
                }
                if (tail_count > 0ULL) {
                    std::cout << ", tail=" << fmt_u64(tail_count);
                }
                std::cout << ")\n";
            } else {
                std::cout << "Range: [" << fmt_seed(start_seed) << ", " << fmt_seed(range_end) << ") lower48\n";
            }
            std::cout << "Batch size: " << fmt_u64(capped_batch_size) << " (" << args.batch_size << ")\n";
            std::cout << "Compiled Stage A mode: requested=" << requested_compiled_stage_a_mode
                      << ", active=" << compiled_stage_a_mode_name(compiled_stage_a_mode)
                      << ", gpu=" << (use_gpu_stage_a ? "yes" : "no") << "\n";
            std::cout << "Completeness: " << query_spec.safety.completeness << "\n";
            std::cout << "Scan mode: " << args.scan_mode << "\n";
            std::cout << "GPU pipeline: requested=" << args.gpu_pipeline
                      << ", active="
                      << (use_gpu_stage_a ? (use_async_gpu_pipeline ? "async-double-buffered" : "sync") : "off")
                      << "\n";
            std::cout << "GPU backend: requested=" << args.gpu_backend
                      << ", active="
                      << (gpu_backend_loaded
                              ? (gpu_backend.active_backend().empty() ? std::string("cuda") : gpu_backend.active_backend())
                              : std::string("none"))
                      << "\n";
            if (gpu_backend_loaded) {
                std::cout << "GPU backend library: " << gpu_backend.loaded_path().string() << "\n";
            }
            std::cout << "GPU async slots: available=" << fmt_u64(gpu_async_slots_available)
                      << ", in_flight=" << fmt_u64(gpu_inflight_batches) << "\n";
            std::cout << "Query mode: version=1, logic=and\n";
        }
        std::string strict_surrogate_mode = query_spec.perf.strict_surrogate;
        if (strict_surrogate_mode == "light-speed") {
            strict_surrogate_mode = "lightspeed";
        }
        if (terminal_mode.load()) {
            std::cout << "Strict surrogate: " << strict_surrogate_mode << "\n";
            std::cout << "Execution mode: requested=" << exec_mode_plan.requested
                      << ", active=" << exec_mode_plan.active
                      << ", simd_width=" << fmt_u64(exec_mode_plan.simd_width)
                      << ", unroll=" << (exec_mode_plan.aggressive_unroll ? "aggressive" : "off")
                      << ", cpu_avx2=" << (exec_mode_plan.caps.avx2 ? "yes" : "no")
                      << ", cpu_avx512=" << (exec_mode_plan.caps.avx512f ? "yes" : "no") << "\n";
            std::cout << "Validation workers: " << fmt_i64(strict_workers) << "\n";
            std::cout << "Java cubiomes validation: "
                      << (java_cubiomes_validation_enabled ? "on" : "off")
                      << " (mode=" << args.java_cubiomes_validation
                      << ", constraints=" << fmt_size(prepared_java_structure_filters.size()) << ")\n";
            if (sculpt_enabled) {
                std::cout << "Sculpt mode: on (pattern=" << query_spec.sculpt.pattern
                          << ", edge=" << query_spec.sculpt.edge
                          << ", center=" << query_spec.sculpt.center
                          << ", radius=" << fmt_i64(query_spec.sculpt.radius)
                          << ", step=" << fmt_i64(query_spec.sculpt.step)
                          << ", dominance=" << fmt_double(query_spec.sculpt.dominance_threshold, 3)
                          << ", allowed=" << fmt_size(query_spec.sculpt.allowed_ids.size()) << ")\n";
            } else {
                std::cout << "Sculpt mode: off\n";
            }
            if (loot_enabled) {
                const uint32_t loot_gate_mul = loot_gate_multiplier(query_spec);
                std::cout << "Loot filters: " << fmt_size(prepared_loot_filters.size())
                          << " (experimental exact post-validation";
                if (loot_gate_mul > 1U) {
                    std::cout << ", surrogate gate x" << fmt_u64(loot_gate_mul);
                }
                std::cout << ")\n";
            } else {
                std::cout << "Loot filters: off\n";
            }
            std::cout << "GPU prefilter constraints: " << fmt_size(gpu_prefilter_plan.constraints.size()) << "\n";
            if (args.plan_diagnostics || args.shadow_compare) {
                std::cout << "Plan diagnostics enabled.\n";
            }
            std::cout << "Running CUDA prefilter + native strict validator...\n";
            if (args.control_stdin) {
                std::cout << "Control stdin: send pause/resume/toggle/snapshot + newline.\n";
            }
        }
        if (args.plan_diagnostics || args.shadow_compare) {
            std::cout << "PLAN_DIAGNOSTICS_START "
                      << describe_plan_diagnostics_json(plan_summary, nullptr, nullptr) << "\n";
        }

        const bool allow_auto_route = compiled_stage_a_mode == CompiledStageAMode::auto_mode;
        const bool allow_runtime_auto_route =
            allow_auto_route && !use_async_gpu_pipeline && !is_exact_policy(query_spec);
        uint32_t gpu_hurt_streak = 0U;
        uint64_t total_gpu_hits = 0;
        uint64_t total_stage_a_survivors = 0;
        uint64_t total_stage_a5_survivors = 0;
        uint64_t total_stage_b_structure_survivors = 0;
        uint64_t total_stage_c_biome_survivors = 0;
        uint64_t total_cpu_verified = 0;
        uint64_t mismatches = 0;
        uint64_t biome_rejected = 0;
        uint64_t sculpt_rejected = 0;
        uint64_t java_worldgen_rejected = 0;
        uint64_t loot_rejected = 0;
        uint64_t cap_pruned_total = 0;
        uint64_t stage_a_reject_total = 0;
        uint64_t stage_a5_reject_total = 0;
        uint64_t stage_b_exact_reject_total = 0;
        uint64_t stage_c_biome_reject_total = 0;
        double total_gpu_kernel_seconds = 0.0;
        double total_gpu_transfer_compaction_seconds = 0.0;
        double total_gpu_seconds = 0.0;
        double total_strict_seconds = 0.0;
        double total_sculpt_seconds = 0.0;
        double total_java_worldgen_seconds = 0.0;
        double total_loot_seconds = 0.0;
        ScannerCompiledBatchStats last_core_stats{};
        bool last_core_stats_available = false;
        std::vector<uint64_t> printed_seeds;
        printed_seeds.reserve(static_cast<size_t>(args.max_print));
        std::ofstream verified_txt;
        if (!args.save_txt.empty()) {
            verified_txt.open(args.save_txt, std::ios::out | std::ios::trunc);
            if (!verified_txt) {
                throw std::runtime_error("Failed to open --save-txt path: " + args.save_txt);
            }
        }

        StdinController stdin_control(args.control_stdin, args, printed_seeds, terminal_mode);

        uint64_t remaining = args.count;
        uint64_t batch_start = start_seed;
        uint64_t batch_idx = 0;
        std::vector<uint64_t> hit_seeds;
        std::vector<uint64_t> valid_out;
        std::vector<uint64_t> java_worldgen_out;
        std::vector<uint64_t> sculpt_out;
        std::vector<uint64_t> loot_out;
        std::vector<uint64_t> structure_settings_out;
        std::vector<NativeQueryMatchOut> java_worldgen_match_buffer;
        std::unordered_set<std::string> java_worldgen_skip_cache;
        std::vector<NativeQueryMatchOut> structure_settings_match_buffer;
        std::vector<StructureSettingsRequest> structure_settings_requests;
        std::vector<uint32_t> structure_settings_request_indices;
        std::vector<bool> structure_settings_results;
        std::vector<uint64_t> structure_settings_current;
        std::vector<uint64_t> structure_settings_next;
        std::vector<NativeQueryMatchOut> loot_match_buffer;
        std::unordered_map<std::string, std::unordered_map<std::string, int>> loot_roll_cache;
        if (java_cubiomes_validation_enabled) {
            java_worldgen_match_buffer.resize(initial_query_match_cap(query_plan.constraints));
        }
        if (structure_settings_enabled) {
            structure_settings_match_buffer.resize(initial_query_match_cap(query_plan.constraints));
        }
        if (loot_enabled) {
            loot_match_buffer.resize(initial_query_match_cap(query_plan.constraints));
        }

        if (use_gpu_stage_a && !gpu_prefilter_plan.constraints.empty()) {
            gpu_backend.filter_multi_into(
                batch_start,
                1,
                gpu_prefilter_plan.regions,
                gpu_prefilter_plan.constraints,
                hit_seeds
            );
        }

        const auto t0 = std::chrono::high_resolution_clock::now();
        const double ui_emit_interval = args.progress_interval <= 0.0
                                            ? 0.50
                                            : std::min<double>(1.00, std::max<double>(0.10, args.progress_interval));
        auto last_ui_emit = t0 - std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(
                                     std::chrono::duration<double>(ui_emit_interval));
        double smoothed_rate = 0.0;
        bool have_smoothed_rate = false;
        auto last_progress = t0 - std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(
                                      std::chrono::duration<double>(args.progress_interval));
        double telemetry_placement_rate_sps = 0.0;
        double telemetry_strict_rate_sps = 0.0;
        double telemetry_queue_saturation_pct = 0.0;
        double telemetry_placement_survivors_per_sec = 0.0;
        double telemetry_strict_verified_per_sec = 0.0;
        double telemetry_gpu_utilization_estimate = 0.0;
        double telemetry_queue_push_wait_time_ms = 0.0;
        double telemetry_placement_gpu_busy_percent = 0.0;
        double telemetry_gpu_batch_kernel_ms = 0.0;
        double telemetry_gpu_host_gap_ms = 0.0;
        double telemetry_gpu_pipeline_fill_percent = 0.0;
        uint64_t telemetry_queue_size = 0ULL;
        uint64_t telemetry_queue_capacity = 0ULL;
        uint64_t telemetry_dropped_survivors = 0ULL;
        uint64_t telemetry_queue_full_events = 0ULL;
        std::mutex stdout_emit_mu;
        auto emit_ui_progress = [&](uint64_t processed, bool done) -> bool {
            const auto now = std::chrono::high_resolution_clock::now();
            if (!done) {
                const double dt = std::chrono::duration<double>(now - last_ui_emit).count();
                if (dt < ui_emit_interval) {
                    return false;
                }
            }
            last_ui_emit = now;
            const double elapsed = std::max(1e-9, std::chrono::duration<double>(now - t0).count());
            const double inst_rate = static_cast<double>(processed) / elapsed;
            if (!have_smoothed_rate) {
                smoothed_rate = inst_rate;
                have_smoothed_rate = true;
            } else {
                smoothed_rate = (smoothed_rate * 0.85) + (inst_rate * 0.15);
            }
            const double pct = (static_cast<double>(processed) / static_cast<double>(args.count)) * 100.0;
            std::ostringstream oss;
            oss << "UI_PROGRESS {\"processed\":" << processed
                << ",\"total\":" << args.count
                << ",\"pct\":" << std::fixed << std::setprecision(4) << pct
                << ",\"rate_sps\":" << std::fixed << std::setprecision(2) << smoothed_rate
                << ",\"stage_a_survivors\":" << total_stage_a_survivors
                << ",\"stage_a5_survivors\":" << total_stage_a5_survivors
                << ",\"stage_b_structure_survivors\":" << total_stage_b_structure_survivors
                << ",\"stage_c_biome_survivors\":" << total_stage_c_biome_survivors
                << ",\"stage_a_rejects\":" << stage_a_reject_total
                << ",\"stage_a5_rejects\":" << stage_a5_reject_total
                << ",\"stage_b_exact_rejects\":" << stage_b_exact_reject_total
                << ",\"stage_c_biome_rejects\":" << stage_c_biome_reject_total
                << ",\"gpu_hits\":" << total_gpu_hits
                << ",\"cpu_hits\":" << total_cpu_verified
                << ",\"mismatches\":" << mismatches
                << ",\"biome_rejected\":" << biome_rejected
                << ",\"sculpt_rejected\":" << sculpt_rejected
                << ",\"java_worldgen_rejected\":" << java_worldgen_rejected
                << ",\"loot_rejected\":" << loot_rejected
                << ",\"cap_pruned\":" << cap_pruned_total
                << ",\"gpu_sec\":" << std::fixed << std::setprecision(6) << total_gpu_seconds
                << ",\"strict_sec\":" << std::fixed << std::setprecision(6) << total_strict_seconds
                << ",\"sculpt_sec\":" << std::fixed << std::setprecision(6) << total_sculpt_seconds
                << ",\"java_worldgen_sec\":" << std::fixed << std::setprecision(6) << total_java_worldgen_seconds
                << ",\"loot_sec\":" << std::fixed << std::setprecision(6) << total_loot_seconds
                << ",\"compiled_stage_a_mode\":\"" << json_escape(compiled_stage_a_mode_name(compiled_stage_a_mode))
                << "\""
                << ",\"gpu_pipeline\":\""
                << json_escape(use_gpu_stage_a ? (use_async_gpu_pipeline ? "async" : "sync") : "off")
                << "\""
                << ",\"gpu_backend\":\""
                << json_escape(
                       gpu_backend_loaded
                           ? (gpu_backend.active_backend().empty() ? std::string("cuda") : gpu_backend.active_backend())
                           : std::string("none"))
                << "\""
                << ",\"gpu_filter_path\":\""
                << json_escape(gpu_backend_loaded ? gpu_backend.loaded_path().string() : std::string())
                << "\""
                << ",\"route_decision\":\"" << json_escape(stage_a_route.route_decision) << "\""
                << ",\"route_reason\":\"" << json_escape(stage_a_route.route_reason) << "\""
                << ",\"scan_mode\":\"" << json_escape(args.scan_mode) << "\""
                << ",\"completeness\":\"" << json_escape(query_spec.safety.completeness) << "\""
                << ",\"raw_placement_seeds_per_sec\":" << std::fixed << std::setprecision(2) << telemetry_placement_rate_sps
                << ",\"placement_rate_sps\":" << std::fixed << std::setprecision(2) << telemetry_placement_rate_sps
                << ",\"strict_rate_sps\":" << std::fixed << std::setprecision(2) << telemetry_strict_rate_sps
                << ",\"strict_consumed_per_sec\":" << std::fixed << std::setprecision(2) << telemetry_strict_rate_sps
                << ",\"queue_depth\":" << telemetry_queue_size
                << ",\"queue_size\":" << telemetry_queue_size
                << ",\"queue_capacity\":" << telemetry_queue_capacity
                << ",\"queue_saturation_percent\":" << std::fixed << std::setprecision(3) << telemetry_queue_saturation_pct
                << ",\"queue_saturation_pct\":" << std::fixed << std::setprecision(3) << telemetry_queue_saturation_pct
                << ",\"strict_worker_count\":" << strict_workers
                << ",\"placement_survivors_per_sec\":" << std::fixed << std::setprecision(2) << telemetry_placement_survivors_per_sec
                << ",\"strict_verified_per_sec\":" << std::fixed << std::setprecision(2) << telemetry_strict_verified_per_sec
                << ",\"dropped_survivors\":" << telemetry_dropped_survivors
                << ",\"queue_full_events\":" << telemetry_queue_full_events
                << ",\"queue_push_wait_time_ms\":" << std::fixed << std::setprecision(3) << telemetry_queue_push_wait_time_ms
                << ",\"placement_gpu_busy_percent\":" << std::fixed << std::setprecision(3) << telemetry_placement_gpu_busy_percent
                << ",\"gpu_busy_percent\":" << std::fixed << std::setprecision(3) << telemetry_placement_gpu_busy_percent
                << ",\"gpu_utilization_estimate\":" << std::fixed << std::setprecision(3) << telemetry_gpu_utilization_estimate
                << ",\"gpu_inflight_batches\":" << gpu_inflight_batches
                << ",\"placement_batch_size\":" << capped_batch_size
                << ",\"gpu_async_slots_available\":" << gpu_async_slots_available
                << ",\"gpu_batch_kernel_ms\":" << std::fixed << std::setprecision(3) << telemetry_gpu_batch_kernel_ms
                << ",\"gpu_host_gap_ms\":" << std::fixed << std::setprecision(3) << telemetry_gpu_host_gap_ms
                << ",\"gpu_pipeline_fill_percent\":" << std::fixed << std::setprecision(3) << telemetry_gpu_pipeline_fill_percent
                << ",\"terminal_mode\":" << (terminal_mode.load() ? "true" : "false")
                << ",\"done\":" << (done ? "true" : "false")
                << "}";
            std::lock_guard<std::mutex> lock(stdout_emit_mu);
            std::cout << oss.str() << "\n";
            return true;
        };
        emit_ui_progress(0ULL, false);
        bool final_progress_emitted = false;

        if (args.scan_mode == "mixed") {
            if (args.shadow_compare) {
                throw std::runtime_error("--scan-mode mixed does not support --shadow-compare.");
            }

            const size_t mixed_queue_batch_size = strict_workers > 1 ? 256U : 32768U;
            BoundedSurvivorBatchQueue survivor_queue(args.mixed_queue_capacity, mixed_queue_batch_size);
            std::atomic<bool> cancel_mixed{false};
            std::atomic<bool> producer_done{false};
            std::atomic<uint32_t> active_workers{static_cast<uint32_t>(strict_workers)};
            std::atomic<uint64_t> mixed_processed{0ULL};
            std::atomic<uint64_t> mixed_placement_survivors{0ULL};
            std::atomic<uint64_t> mixed_dropped_survivors{0ULL};
            std::atomic<uint64_t> mixed_strict_input{0ULL};
            std::atomic<uint64_t> mixed_verified{0ULL};
            std::atomic<uint64_t> mixed_mismatches{0ULL};
            std::atomic<uint64_t> mixed_biome_rejected{0ULL};
            std::atomic<uint64_t> mixed_java_rejected{0ULL};
            std::atomic<uint64_t> mixed_sculpt_rejected{0ULL};
            std::atomic<uint64_t> mixed_loot_rejected{0ULL};
            std::atomic<uint64_t> mixed_cap_pruned{0ULL};
            std::atomic<uint64_t> mixed_throttle_events{0ULL};
            std::atomic<uint64_t> mixed_queue_full_events{0ULL};
            std::atomic<uint64_t> mixed_queue_push_wait_ns{0ULL};
            std::atomic<uint64_t> mixed_placement_gpu_ns{0ULL};
            std::atomic<uint64_t> mixed_gpu_kernel_ns{0ULL};
            std::atomic<uint64_t> mixed_gpu_host_gap_ns{0ULL};
            std::atomic<uint64_t> mixed_gpu_batch_count{0ULL};
            std::atomic<uint32_t> mixed_pending_gpu_batches{0U};
            std::atomic<uint64_t> mixed_strict_ns{0ULL};
            std::atomic<uint64_t> mixed_java_ns{0ULL};
            std::atomic<uint64_t> mixed_sculpt_ns{0ULL};
            std::atomic<uint64_t> mixed_loot_ns{0ULL};
            std::atomic<uint32_t> mixed_strict_consume_log_count{0U};
            std::atomic<uint32_t> mixed_verified_log_count{0U};

            std::mutex mixed_debug_log_mu;
            std::mutex mixed_shared_java_validation_mu;
            auto mixed_debug_log_line = [&](auto write_line) {
                std::ostringstream oss;
                write_line(oss);
                std::lock_guard<std::mutex> lock(mixed_debug_log_mu);
                std::cerr << oss.str() << "\n";
            };

            if (requested_strict_workers != strict_workers) {
                mixed_debug_log_line([&](std::ostream &os) {
                    os << "[mixed] sparse strict gate active; validation workers capped "
                       << fmt_i64(requested_strict_workers) << " -> " << fmt_i64(strict_workers);
                });
            }

            mixed_debug_log_line([&](std::ostream &os) {
                os << "[mixed] placement pipeline started";
            });
            mixed_debug_log_line([&](std::ostream &os) {
                os << "[mixed] selected execution route=" << stage_a_route.route_decision
                   << " reason=" << stage_a_route.route_reason
                   << " stage_a_mode=" << compiled_stage_a_mode_name(route_stage_a_mode)
                   << " gpu_stage_active=" << (use_gpu_stage_a ? "yes" : "no")
                   << " gpu_pipeline=" << (use_gpu_stage_a ? (use_async_gpu_pipeline ? "async" : "sync") : "off")
                   << " gpu_async_slots=" << fmt_u64(gpu_async_slots_available)
                   << " gpu_inflight_batches=" << fmt_u64(gpu_inflight_batches)
                   << " placement_batch=" << fmt_u64(capped_batch_size)
                   << " queue_capacity=" << fmt_u64(args.mixed_queue_capacity)
                   << " queue_batch=" << fmt_size(mixed_queue_batch_size)
                   << " strict_workers=" << fmt_i64(strict_workers);
            });
            if (use_gpu_stage_a) {
                mixed_debug_log_line([&](std::ostream &os) {
                    os << "[mixed] GPU stage active constraints=" << fmt_size(gpu_prefilter_plan.constraints.size())
                       << " regions=" << fmt_size(gpu_prefilter_plan.regions.size());
                });
            }

            std::mutex mixed_error_mu;
            std::exception_ptr mixed_error;
            auto set_mixed_error = [&](std::exception_ptr error) {
                std::lock_guard<std::mutex> lock(mixed_error_mu);
                if (!mixed_error) {
                    mixed_error = error;
                }
                cancel_mixed.store(true);
                survivor_queue.close();
            };

            auto add_elapsed_ns = [](std::atomic<uint64_t> &target, auto start_time) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::high_resolution_clock::now() - start_time);
                target.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, elapsed.count())));
            };

            std::mutex verified_queue_mu;
            std::deque<std::vector<uint64_t>> verified_batches;
            auto push_verified_batch = [&](std::vector<uint64_t> &&batch) {
                if (batch.empty()) {
                    return;
                }
                std::lock_guard<std::mutex> lock(verified_queue_mu);
                verified_batches.push_back(std::move(batch));
            };
            auto drain_verified_batches = [&]() {
                std::deque<std::vector<uint64_t>> local;
                {
                    std::lock_guard<std::mutex> lock(verified_queue_mu);
                    local.swap(verified_batches);
                }
                for (const std::vector<uint64_t> &batch : local) {
                    for (uint64_t seed : batch) {
                        if (static_cast<int>(printed_seeds.size()) < args.max_print) {
                            printed_seeds.push_back(seed);
                            if (args.stream_seeds) {
                                std::lock_guard<std::mutex> lock(stdout_emit_mu);
                                emit_seed_found_line(seed, args, "strict");
                            }
                        }
                        if (verified_txt) {
                            write_seed_txt_line(verified_txt, seed, args);
                        }
                    }
                }
            };

            mixed_debug_log_line([&](std::ostream &os) {
                os << "[mixed] queue producer startup";
            });
            std::thread producer([&]() {
                try {
                    uint64_t local_start = batch_start;
                    uint64_t local_remaining = remaining;
                    std::vector<uint64_t> survivors;
                    uint32_t placement_streamed = 0U;
                    uint32_t submit_log_count = 0U;
                    uint32_t placement_batch_log_count = 0U;
                    uint32_t survivor_emit_log_count = 0U;
                    uint32_t queue_push_log_count = 0U;
                    mixed_debug_log_line([&](std::ostream &os) {
                        os << "[mixed] placement producer started start=" << fmt_u64(local_start)
                           << " remaining=" << fmt_u64(local_remaining)
                           << " batch=" << fmt_u64(capped_batch_size)
                           << " async=" << (use_async_gpu_pipeline ? "yes" : "no");
                    });
                    struct PendingGpuBatch {
                        bool active = false;
                        uint32_t slot_id = 0U;
                        uint64_t start_seed = 0ULL;
                        uint64_t count = 0ULL;
                    };
                    std::deque<PendingGpuBatch> pending_gpu_batches;
                    uint32_t next_async_slot = 0U;
                    uint64_t async_next_batch_start = local_start;
                    uint64_t async_remaining = local_remaining;
                    auto submit_next_async_gpu_batch = [&]() -> bool {
                        if (!use_async_gpu_pipeline || async_remaining == 0ULL || cancel_mixed.load()) {
                            return false;
                        }
                        if (pending_gpu_batches.size() >= gpu_inflight_batches) {
                            return false;
                        }
                        const uint64_t submit_count = std::min<uint64_t>(async_remaining, capped_batch_size);
                        const uint32_t slot_id = next_async_slot;
                        gpu_backend.submit_batch(
                            slot_id,
                            async_next_batch_start,
                            submit_count,
                            gpu_prefilter_plan.regions,
                            gpu_prefilter_plan.constraints
                        );
                        if (submit_log_count < 4U) {
                            mixed_debug_log_line([&](std::ostream &os) {
                                os << "[mixed] GPU batch submitted slot=" << fmt_u64(slot_id)
                                   << " start=" << fmt_u64(async_next_batch_start)
                                   << " count=" << fmt_u64(submit_count);
                            });
                            ++submit_log_count;
                        }
                        PendingGpuBatch pending{};
                        pending.active = true;
                        pending.slot_id = slot_id;
                        pending.start_seed = async_next_batch_start;
                        pending.count = submit_count;
                        pending_gpu_batches.push_back(pending);
                        mixed_pending_gpu_batches.store(static_cast<uint32_t>(pending_gpu_batches.size()));
                        async_next_batch_start = (async_next_batch_start + submit_count) & JAVA_MASK;
                        async_remaining -= submit_count;
                        next_async_slot = gpu_inflight_batches == 0U
                            ? 0U
                            : ((next_async_slot + 1U) % gpu_inflight_batches);
                        return true;
                    };
                    if (use_async_gpu_pipeline) {
                        while (pending_gpu_batches.size() < gpu_inflight_batches &&
                               submit_next_async_gpu_batch()) {
                        }
                    }
                    while (local_remaining > 0ULL && !cancel_mixed.load()) {
                        while (stdin_control.paused() && !cancel_mixed.load()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }

                        uint64_t submit_count = std::min<uint64_t>(local_remaining, capped_batch_size);
                        if (use_async_gpu_pipeline) {
                            if (pending_gpu_batches.empty()) {
                                throw std::runtime_error("Mixed async GPU pipeline had no pending batch to collect.");
                            }
                            const PendingGpuBatch pending_gpu_batch = pending_gpu_batches.front();
                            pending_gpu_batches.pop_front();
                            mixed_pending_gpu_batches.store(static_cast<uint32_t>(pending_gpu_batches.size()));
                            local_start = pending_gpu_batch.start_seed;
                            submit_count = pending_gpu_batch.count;
                            double batch_gpu_kernel_seconds = 0.0;
                            double batch_gpu_transfer_seconds = 0.0;
                            gpu_backend.collect_batch(
                                pending_gpu_batch.slot_id,
                                survivors,
                                batch_gpu_kernel_seconds,
                                batch_gpu_transfer_seconds
                            );
                            const double batch_gpu_seconds =
                                std::max(0.0, batch_gpu_kernel_seconds) + std::max(0.0, batch_gpu_transfer_seconds);
                            mixed_placement_gpu_ns.fetch_add(static_cast<uint64_t>(batch_gpu_seconds * 1e9));
                            mixed_gpu_kernel_ns.fetch_add(static_cast<uint64_t>(
                                std::max(0.0, batch_gpu_kernel_seconds) * 1e9));
                            mixed_gpu_batch_count.fetch_add(1ULL);
                            submit_next_async_gpu_batch();
                        } else {
                            const auto gpu_t0 = std::chrono::high_resolution_clock::now();
                            gpu_backend.filter_multi_into(
                                local_start,
                                submit_count,
                                gpu_prefilter_plan.regions,
                                gpu_prefilter_plan.constraints,
                                survivors
                            );
                            add_elapsed_ns(mixed_placement_gpu_ns, gpu_t0);
                            mixed_gpu_kernel_ns.fetch_add(static_cast<uint64_t>(
                                std::max(0.0, std::chrono::duration<double>(
                                    std::chrono::high_resolution_clock::now() - gpu_t0).count()) * 1e9));
                            mixed_gpu_batch_count.fetch_add(1ULL);
                        }
                        const auto host_gap_t0 = std::chrono::high_resolution_clock::now();
                        if (placement_batch_log_count < 8U) {
                            mixed_debug_log_line([&](std::ostream &os) {
                                os << "[mixed] placement batch complete start=" << fmt_u64(local_start)
                                   << " count=" << fmt_u64(submit_count)
                                   << " survivors=" << fmt_size(survivors.size());
                            });
                            ++placement_batch_log_count;
                        }
                        mixed_processed.fetch_add(submit_count);
                        mixed_placement_survivors.fetch_add(static_cast<uint64_t>(survivors.size()));
                        if (!is_exact_policy(query_spec) &&
                            args.mixed_survivor_cap > 0U && survivors.size() > args.mixed_survivor_cap) {
                            const uint64_t drop =
                                static_cast<uint64_t>(survivors.size() - static_cast<size_t>(args.mixed_survivor_cap));
                            mixed_dropped_survivors.fetch_add(drop);
                            survivors.resize(args.mixed_survivor_cap);
                        }
                        if (!is_exact_policy(query_spec) && args.mixed_adaptive_throttling && !survivors.empty()) {
                            const double saturation = survivor_queue.saturation();
                            if (saturation >= 0.90) {
                                ++mixed_throttle_events;
                                const uint64_t qsize = survivor_queue.size();
                                const uint64_t available =
                                    qsize >= survivor_queue.capacity() ? 0ULL : survivor_queue.capacity() - qsize;
                                size_t emit_limit = static_cast<size_t>(std::min<uint64_t>(
                                    available,
                                    static_cast<uint64_t>(survivors.size())));
                                if (saturation >= 0.98) {
                                    emit_limit = 0U;
                                }
                                if (emit_limit < survivors.size()) {
                                    mixed_dropped_survivors.fetch_add(
                                        static_cast<uint64_t>(survivors.size() - emit_limit));
                                    survivors.resize(emit_limit);
                                }
                            }
                        }

                        if (!survivors.empty() && !cancel_mixed.load()) {
                            if (survivor_emit_log_count < 8U) {
                                mixed_debug_log_line([&](std::ostream &os) {
                                    os << "[mixed] survivor emitted count=" << fmt_size(survivors.size())
                                       << " first=" << fmt_u64(survivors.front());
                                });
                                ++survivor_emit_log_count;
                            }
                            if (args.stream_seeds && placement_streamed < static_cast<uint32_t>(args.max_print)) {
                                for (uint64_t seed : survivors) {
                                    if (placement_streamed >= static_cast<uint32_t>(args.max_print)) {
                                        break;
                                    }
                                    {
                                        std::lock_guard<std::mutex> lock(stdout_emit_mu);
                                        emit_seed_found_line(seed, args, "placement");
                                    }
                                    ++placement_streamed;
                                }
                            }

                            for (uint64_t &seed : survivors) {
                                seed &= JAVA_MASK;
                            }
                            const size_t pushed_count = survivors.size();
                            uint64_t queue_dropped = 0ULL;
                            uint64_t queue_full_events = 0ULL;
                            uint64_t queue_push_wait_ns = 0ULL;
                            if (!survivor_queue.try_push_batch(
                                    std::move(survivors),
                                    cancel_mixed,
                                    queue_dropped,
                                    queue_full_events,
                                    queue_push_wait_ns,
                                    !is_exact_policy(query_spec))) {
                                break;
                            }
                            if (queue_dropped > 0ULL) {
                                mixed_dropped_survivors.fetch_add(queue_dropped);
                            }
                            if (queue_full_events > 0ULL) {
                                mixed_queue_full_events.fetch_add(queue_full_events);
                            }
                            if (queue_push_wait_ns > 0ULL) {
                                mixed_queue_push_wait_ns.fetch_add(queue_push_wait_ns);
                            }
                            if (queue_push_log_count < 8U) {
                                mixed_debug_log_line([&](std::ostream &os) {
                                    os << "[mixed] queue push success pushed=" << fmt_size(pushed_count)
                                       << " dropped=" << fmt_u64(queue_dropped)
                                       << " depth=" << fmt_u64(survivor_queue.size())
                                       << "/" << fmt_u64(survivor_queue.capacity());
                                });
                                ++queue_push_log_count;
                            }
                        }

                        add_elapsed_ns(mixed_gpu_host_gap_ns, host_gap_t0);

                        local_start = (local_start + submit_count) & JAVA_MASK;
                        local_remaining -= submit_count;
                    }
                    producer_done.store(true);
                    mixed_pending_gpu_batches.store(0U);
                    survivor_queue.close();
                } catch (...) {
                    producer_done.store(true);
                    mixed_pending_gpu_batches.store(0U);
                    set_mixed_error(std::current_exception());
                }
            });

            const bool mixed_strict_input_is_post_stage_a =
                use_gpu_stage_a &&
                !gpu_prefilter_plan.constraints.empty() &&
                gpu_prefilter_plan.constraints.size() == query_plan.constraints.size();
            std::vector<std::thread> workers;
            workers.reserve(static_cast<size_t>(strict_workers));
            for (int worker_index = 0; worker_index < strict_workers; ++worker_index) {
                workers.emplace_back([&, worker_index]() {
                    (void)worker_index;
                    try {
                        std::vector<uint64_t> batch;
                        std::vector<uint64_t> valid;
                        std::vector<uint64_t> post_a;
                        std::vector<uint64_t> post_b;
                        std::vector<NativeQueryMatchOut> local_java_match_buffer;
                        std::unordered_set<std::string> local_java_skip_cache;
                        std::vector<NativeQueryMatchOut> local_structure_settings_match_buffer;
                        std::vector<StructureSettingsRequest> local_structure_settings_requests;
                        std::vector<uint32_t> local_structure_settings_request_indices;
                        std::vector<bool> local_structure_settings_results;
                        std::vector<uint64_t> local_structure_settings_current;
                        std::vector<uint64_t> local_structure_settings_next;
                        std::vector<NativeQueryMatchOut> local_loot_match_buffer;
                        std::unordered_map<std::string, std::unordered_map<std::string, int>> local_loot_roll_cache;
                        std::vector<LootCheckRequest> local_loot_check_requests;
                        std::vector<uint32_t> local_loot_check_seed_indices;
                        std::vector<bool> local_loot_check_results;
                        std::vector<uint64_t> local_loot_current;
                        std::vector<uint64_t> local_loot_next;
                        if (java_cubiomes_validation_enabled) {
                            local_java_match_buffer.resize(initial_query_match_cap(query_plan.constraints));
                        }
                        if (structure_settings_enabled) {
                            local_structure_settings_match_buffer.resize(initial_query_match_cap(query_plan.constraints));
                        }
                        if (loot_enabled) {
                            local_loot_match_buffer.resize(initial_query_match_cap(query_plan.constraints));
                        }
                        std::unique_ptr<LootValidationServerClient> local_loot_client;
                        auto worker_loot_client = [&]() -> LootValidationServerClient & {
                            if (!local_loot_client) {
                                local_loot_client.reset(new LootValidationServerClient());
                                std::string loot_error;
                                if (!local_loot_client->start(repo_root, loot_error)) {
                                    throw std::runtime_error("Failed to start worker LootValidationServer: " + loot_error);
                                }
                            }
                            return *local_loot_client;
                        };

                        while (!cancel_mixed.load() && survivor_queue.pop_batch(batch)) {
                            if (batch.empty()) {
                                continue;
                            }
                            const uint32_t consume_log_index = mixed_strict_consume_log_count.fetch_add(1U);
                            if (consume_log_index < 8U) {
                                mixed_debug_log_line([&](std::ostream &os) {
                                    os << "[mixed] strict worker consume worker=" << fmt_i64(worker_index)
                                       << " batch=" << fmt_size(batch.size());
                                });
                            }
                            mixed_strict_input.fetch_add(static_cast<uint64_t>(batch.size()));
                            if (valid.size() < batch.size()) {
                                valid.resize(batch.size());
                            }
                            uint32_t valid_count = 0U;
                            uint32_t mismatch_count = 0U;
                            uint32_t biome_reject_count = 0U;
                            uint32_t cap_pruned = 0U;
                            const auto strict_t0 = std::chrono::high_resolution_clock::now();
                            const uint32_t strict_input_count = static_cast<uint32_t>(std::min<size_t>(
                                batch.size(),
                                static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
                            const int rc = mixed_strict_input_is_post_stage_a
                                ? query_plan.validate_batch_post_stage_a(
                                      batch.data(),
                                      strict_input_count,
                                      1U,
                                      valid.data(),
                                      &valid_count,
                                      &mismatch_count,
                                      &biome_reject_count,
                                      &cap_pruned)
                                : query_plan.validate_batch(
                                      batch.data(),
                                      strict_input_count,
                                      1U,
                                      valid.data(),
                                      &valid_count,
                                      &mismatch_count,
                                      &biome_reject_count,
                                      &cap_pruned);
                            add_elapsed_ns(mixed_strict_ns, strict_t0);
                            if (rc != 0) {
                                std::ostringstream oss;
                                oss << "scanner_core_validate_query_batch failed with code " << fmt_i64(rc);
                                throw std::runtime_error(oss.str());
                            }
                            mixed_mismatches.fetch_add(mismatch_count);
                            mixed_biome_rejected.fetch_add(biome_reject_count);
                            mixed_cap_pruned.fetch_add(cap_pruned);

                            if (java_cubiomes_validation_enabled && valid_count > 0U) {
                                if (post_a.size() < valid_count) {
                                    post_a.resize(valid_count);
                                }
                                uint32_t java_valid_count = 0U;
                                uint32_t java_reject_count = 0U;
                                const auto java_t0 = std::chrono::high_resolution_clock::now();
                                auto validate_java_outputs = [&](LootValidationServerClient &client) {
                                    for (uint32_t i = 0; i < valid_count; ++i) {
                                        if (validate_java_structure_matches_for_seed(
                                                valid[i],
                                                prepared_java_structure_filters,
                                                loot_version_token,
                                                query_plan,
                                                client,
                                                local_java_match_buffer,
                                                local_java_skip_cache)) {
                                            post_a[java_valid_count++] = valid[i];
                                        } else {
                                            ++java_reject_count;
                                        }
                                    }
                                };
                                if (!loot_enabled && !structure_settings_enabled) {
                                    std::lock_guard<std::mutex> java_lock(mixed_shared_java_validation_mu);
                                    validate_java_outputs(loot_client);
                                } else {
                                    validate_java_outputs(worker_loot_client());
                                }
                                add_elapsed_ns(mixed_java_ns, java_t0);
                                mixed_java_rejected.fetch_add(java_reject_count);
                                if (java_valid_count > 0U) {
                                    std::copy_n(post_a.data(), java_valid_count, valid.data());
                                }
                                valid_count = java_valid_count;
                            }

                            if (structure_settings_enabled && valid_count > 0U) {
                                if (post_a.size() < valid_count) {
                                    post_a.resize(valid_count);
                                }
                                const auto settings_t0 = std::chrono::high_resolution_clock::now();
                                LootValidationServerClient &client = worker_loot_client();
                                const uint32_t settings_valid_count = validate_structure_settings_for_seed_batch(
                                    valid.data(),
                                    valid_count,
                                    prepared_structure_settings_filters,
                                    loot_version_token,
                                    query_plan,
                                    client,
                                    local_structure_settings_match_buffer,
                                    local_structure_settings_requests,
                                    local_structure_settings_request_indices,
                                    local_structure_settings_results,
                                    local_structure_settings_current,
                                    local_structure_settings_next,
                                    post_a.data());
                                add_elapsed_ns(mixed_java_ns, settings_t0);
                                mixed_java_rejected.fetch_add(valid_count - settings_valid_count);
                                if (settings_valid_count > 0U) {
                                    std::copy_n(post_a.data(), settings_valid_count, valid.data());
                                }
                                valid_count = settings_valid_count;
                            }

                            if (sculpt_enabled && valid_count > 0U) {
                                if (post_a.size() < valid_count) {
                                    post_a.resize(valid_count);
                                }
                                uint32_t sculpt_valid_count = 0U;
                                uint32_t sculpt_reject_count = 0U;
                                const auto sculpt_t0 = std::chrono::high_resolution_clock::now();
                                const int sculpt_rc = scanner_core_filter_sculpt_batch(
                                    valid.data(),
                                    valid_count,
                                    &native_sculpt,
                                    native_sculpt_allowed.data(),
                                    static_cast<uint32_t>(native_sculpt_allowed.size()),
                                    cubi_mc_version,
                                    1U,
                                    post_a.data(),
                                    &sculpt_valid_count,
                                    &sculpt_reject_count
                                );
                                add_elapsed_ns(mixed_sculpt_ns, sculpt_t0);
                                if (sculpt_rc != 0) {
                                    std::ostringstream oss;
                                    oss << "scanner_core_filter_sculpt_batch failed with code " << fmt_i64(sculpt_rc);
                                    throw std::runtime_error(oss.str());
                                }
                                mixed_sculpt_rejected.fetch_add(sculpt_reject_count);
                                if (sculpt_valid_count > 0U) {
                                    std::copy_n(post_a.data(), sculpt_valid_count, valid.data());
                                }
                                valid_count = sculpt_valid_count;
                            }

                            if (loot_enabled && valid_count > 0U) {
                                if (post_b.size() < valid_count) {
                                    post_b.resize(valid_count);
                                }
                                uint32_t loot_valid_count = 0U;
                                const auto loot_t0 = std::chrono::high_resolution_clock::now();
                                LootValidationServerClient &client = worker_loot_client();
                                loot_valid_count = validate_loot_filters_for_seed_batch(
                                    valid.data(),
                                    valid_count,
                                    prepared_loot_filters,
                                    loot_version_token,
                                    query_plan,
                                    client,
                                    local_loot_match_buffer,
                                    local_loot_check_requests,
                                    local_loot_check_seed_indices,
                                    local_loot_check_results,
                                    local_loot_current,
                                    local_loot_next,
                                    post_b.data());
                                const uint32_t loot_reject_count = valid_count - loot_valid_count;
                                (void)local_loot_roll_cache;
                                add_elapsed_ns(mixed_loot_ns, loot_t0);
                                mixed_loot_rejected.fetch_add(loot_reject_count);
                                if (loot_valid_count > 0U) {
                                    std::copy_n(post_b.data(), loot_valid_count, valid.data());
                                }
                                valid_count = loot_valid_count;
                            }

                            if (valid_count > 0U) {
                                std::vector<uint64_t> verified(valid.begin(), valid.begin() + valid_count);
                                mixed_verified.fetch_add(valid_count);
                                const uint32_t verified_log_index = mixed_verified_log_count.fetch_add(1U);
                                if (verified_log_index < 8U) {
                                    mixed_debug_log_line([&](std::ostream &os) {
                                        os << "[mixed] verified seed emitted count=" << fmt_u64(valid_count)
                                           << " first=" << fmt_u64(verified.front());
                                    });
                                }
                                push_verified_batch(std::move(verified));
                            }
                        }
                    } catch (...) {
                        set_mixed_error(std::current_exception());
                    }
                    active_workers.fetch_sub(1U);
                });
            }

            uint64_t last_processed_sample = 0ULL;
            uint64_t last_survivor_sample = 0ULL;
            uint64_t last_strict_input_sample = 0ULL;
            uint64_t last_verified_sample = 0ULL;
            uint64_t last_queue_push_wait_ns_sample = 0ULL;
            uint64_t last_placement_gpu_ns_sample = 0ULL;
            uint64_t last_gpu_kernel_ns_sample = 0ULL;
            uint64_t last_gpu_host_gap_ns_sample = 0ULL;
            uint64_t last_gpu_batch_count_sample = 0ULL;
            auto last_mixed_sample = std::chrono::high_resolution_clock::now();

            while (true) {
                drain_verified_batches();
                {
                    std::lock_guard<std::mutex> lock(mixed_error_mu);
                    if (mixed_error) {
                        cancel_mixed.store(true);
                    }
                }
                const auto now = std::chrono::high_resolution_clock::now();
                const double sample_dt =
                    std::max(1e-6, std::chrono::duration<double>(now - last_mixed_sample).count());
                const uint64_t processed_now = mixed_processed.load();
                const uint64_t survivors_now = mixed_placement_survivors.load();
                const uint64_t strict_input_now = mixed_strict_input.load();
                const uint64_t verified_now = mixed_verified.load();
                const uint64_t queue_push_wait_ns_now = mixed_queue_push_wait_ns.load();
                const uint64_t placement_gpu_ns_now = mixed_placement_gpu_ns.load();
                const uint64_t gpu_kernel_ns_now = mixed_gpu_kernel_ns.load();
                const uint64_t gpu_host_gap_ns_now = mixed_gpu_host_gap_ns.load();
                const uint64_t gpu_batch_count_now = mixed_gpu_batch_count.load();
                const uint64_t gpu_batch_delta =
                    gpu_batch_count_now > last_gpu_batch_count_sample
                        ? gpu_batch_count_now - last_gpu_batch_count_sample
                        : 0ULL;
                telemetry_placement_rate_sps =
                    static_cast<double>(processed_now - last_processed_sample) / sample_dt;
                telemetry_placement_survivors_per_sec =
                    static_cast<double>(survivors_now - last_survivor_sample) / sample_dt;
                telemetry_strict_rate_sps =
                    static_cast<double>(strict_input_now - last_strict_input_sample) / sample_dt;
                telemetry_strict_verified_per_sec =
                    static_cast<double>(verified_now - last_verified_sample) / sample_dt;
                telemetry_queue_push_wait_time_ms =
                    static_cast<double>(queue_push_wait_ns_now - last_queue_push_wait_ns_sample) / 1e6;
                telemetry_placement_gpu_busy_percent = std::min(
                    100.0,
                    (static_cast<double>(placement_gpu_ns_now - last_placement_gpu_ns_sample) /
                     (sample_dt * 1e9)) *
                        100.0);
                telemetry_gpu_batch_kernel_ms = gpu_batch_delta > 0ULL
                    ? (static_cast<double>(gpu_kernel_ns_now - last_gpu_kernel_ns_sample) /
                       static_cast<double>(gpu_batch_delta)) /
                          1e6
                    : 0.0;
                telemetry_gpu_host_gap_ms = gpu_batch_delta > 0ULL
                    ? (static_cast<double>(gpu_host_gap_ns_now - last_gpu_host_gap_ns_sample) /
                       static_cast<double>(gpu_batch_delta)) /
                          1e6
                    : 0.0;
                telemetry_gpu_pipeline_fill_percent =
                    gpu_inflight_batches == 0U
                        ? 0.0
                        : (static_cast<double>(mixed_pending_gpu_batches.load()) /
                           static_cast<double>(gpu_inflight_batches)) *
                              100.0;

                telemetry_queue_size = survivor_queue.size();
                telemetry_queue_capacity = survivor_queue.capacity();
                telemetry_queue_saturation_pct = survivor_queue.saturation() * 100.0;
                telemetry_dropped_survivors = mixed_dropped_survivors.load();
                telemetry_queue_full_events = mixed_queue_full_events.load();
                telemetry_gpu_utilization_estimate =
                    producer_done.load() ? 0.0 : telemetry_placement_gpu_busy_percent;

                total_stage_a_survivors = survivors_now;
                total_gpu_hits = survivors_now;
                stage_a_reject_total = processed_now > survivors_now ? processed_now - survivors_now : 0ULL;
                total_stage_a5_survivors = strict_input_now;
                total_stage_b_structure_survivors = verified_now + mixed_biome_rejected.load();
                total_stage_c_biome_survivors = verified_now;
                total_cpu_verified = verified_now;
                mismatches = mixed_mismatches.load();
                biome_rejected = mixed_biome_rejected.load();
                sculpt_rejected = mixed_sculpt_rejected.load();
                java_worldgen_rejected = mixed_java_rejected.load();
                loot_rejected = mixed_loot_rejected.load();
                cap_pruned_total = mixed_cap_pruned.load();
                stage_b_exact_reject_total = mismatches;
                stage_c_biome_reject_total = biome_rejected;
                total_strict_seconds = static_cast<double>(mixed_strict_ns.load()) / 1e9;
                total_gpu_seconds = static_cast<double>(mixed_placement_gpu_ns.load()) / 1e9;
                total_java_worldgen_seconds = static_cast<double>(mixed_java_ns.load()) / 1e9;
                total_sculpt_seconds = static_cast<double>(mixed_sculpt_ns.load()) / 1e9;
                total_loot_seconds = static_cast<double>(mixed_loot_ns.load()) / 1e9;

                const bool done = producer_done.load() && active_workers.load() == 0U;
                const bool emitted_progress = emit_ui_progress(processed_now, done);
                if (emitted_progress) {
                    last_processed_sample = processed_now;
                    last_survivor_sample = survivors_now;
                    last_strict_input_sample = strict_input_now;
                    last_verified_sample = verified_now;
                    last_queue_push_wait_ns_sample = queue_push_wait_ns_now;
                    last_placement_gpu_ns_sample = placement_gpu_ns_now;
                    last_gpu_kernel_ns_sample = gpu_kernel_ns_now;
                    last_gpu_host_gap_ns_sample = gpu_host_gap_ns_now;
                    last_gpu_batch_count_sample = gpu_batch_count_now;
                    last_mixed_sample = now;
                }
                if (done && emitted_progress) {
                    final_progress_emitted = true;
                }
                if (done) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (producer.joinable()) {
                producer.join();
            }
            for (std::thread &worker : workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            drain_verified_batches();
            {
                std::lock_guard<std::mutex> lock(mixed_error_mu);
                if (mixed_error) {
                    std::rethrow_exception(mixed_error);
                }
            }
            total_stage_a_survivors = mixed_placement_survivors.load();
            total_gpu_hits = total_stage_a_survivors;
            stage_a_reject_total =
                mixed_processed.load() > total_stage_a_survivors ? mixed_processed.load() - total_stage_a_survivors : 0ULL;
            total_stage_a5_survivors = mixed_strict_input.load();
            total_stage_b_structure_survivors = mixed_verified.load() + mixed_biome_rejected.load();
            total_stage_c_biome_survivors = mixed_verified.load();
            total_cpu_verified = mixed_verified.load();
            mismatches = mixed_mismatches.load();
            biome_rejected = mixed_biome_rejected.load();
            sculpt_rejected = mixed_sculpt_rejected.load();
            java_worldgen_rejected = mixed_java_rejected.load();
            loot_rejected = mixed_loot_rejected.load();
            cap_pruned_total = mixed_cap_pruned.load();
            stage_b_exact_reject_total = mismatches;
            stage_c_biome_reject_total = biome_rejected;
            total_strict_seconds = static_cast<double>(mixed_strict_ns.load()) / 1e9;
            total_gpu_seconds = static_cast<double>(mixed_placement_gpu_ns.load()) / 1e9;
            total_java_worldgen_seconds = static_cast<double>(mixed_java_ns.load()) / 1e9;
            total_sculpt_seconds = static_cast<double>(mixed_sculpt_ns.load()) / 1e9;
            total_loot_seconds = static_cast<double>(mixed_loot_ns.load()) / 1e9;
        } else {
        struct PendingGpuBatch {
            uint32_t slot_id = 0U;
            uint64_t start_seed = 0ULL;
            uint64_t count = 0ULL;
        };
        std::deque<PendingGpuBatch> pending_gpu_batches;
        uint32_t next_async_slot = 0U;
        uint64_t async_next_batch_start = batch_start;
        uint64_t async_remaining = remaining;
        auto submit_next_async_gpu_batch = [&]() -> bool {
            if (!use_async_gpu_pipeline || !use_gpu_stage_a || async_remaining == 0ULL) {
                return false;
            }
            if (pending_gpu_batches.size() >= gpu_inflight_batches) {
                return false;
            }
            const uint64_t submit_count = std::min<uint64_t>(async_remaining, capped_batch_size);
            const uint32_t slot_id = next_async_slot;
            gpu_backend.submit_batch(
                slot_id,
                async_next_batch_start,
                submit_count,
                gpu_prefilter_plan.regions,
                gpu_prefilter_plan.constraints
            );
            pending_gpu_batches.push_back(PendingGpuBatch{slot_id, async_next_batch_start, submit_count});
            async_next_batch_start += submit_count;
            async_remaining -= submit_count;
            next_async_slot = gpu_inflight_batches == 0U
                ? 0U
                : ((next_async_slot + 1U) % gpu_inflight_batches);
            return true;
        };
        if (use_async_gpu_pipeline) {
            while (pending_gpu_batches.size() < gpu_inflight_batches &&
                   submit_next_async_gpu_batch()) {
            }
        }

        while (remaining > 0) {
            while (stdin_control.paused()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            ++batch_idx;
            const uint64_t this_batch = std::min<uint64_t>(remaining, capped_batch_size);
            if (terminal_mode.load() && batch_idx == 1) {
                std::cout << "Batch 1: starting (size=" << fmt_u64(this_batch) << ")...\n";
            }
            RunBatchStats batch_stats{};
            batch_stats.input_seed_count = this_batch;
            double batch_gpu_seconds = 0.0;
            double batch_gpu_kernel_seconds = 0.0;
            double batch_gpu_transfer_compaction_seconds = 0.0;
            if (use_gpu_stage_a) {
                if (use_async_gpu_pipeline) {
                    if (pending_gpu_batches.empty() && !submit_next_async_gpu_batch()) {
                        throw std::runtime_error("Async GPU pipeline had no pending batch to collect.");
                    }
                    const PendingGpuBatch pending_gpu_batch = pending_gpu_batches.front();
                    pending_gpu_batches.pop_front();
                    if (pending_gpu_batch.start_seed != batch_start || pending_gpu_batch.count != this_batch) {
                        std::ostringstream oss;
                        oss << "Async GPU pipeline batch order mismatch: expected start=" << fmt_seed(batch_start)
                            << " count=" << fmt_u64(this_batch)
                            << ", pending start=" << fmt_seed(pending_gpu_batch.start_seed)
                            << " count=" << fmt_u64(pending_gpu_batch.count);
                        throw std::runtime_error(oss.str());
                    }
                    gpu_backend.collect_batch(
                        pending_gpu_batch.slot_id,
                        hit_seeds,
                        batch_gpu_kernel_seconds,
                        batch_gpu_transfer_compaction_seconds
                    );
                    batch_gpu_seconds = batch_gpu_kernel_seconds + batch_gpu_transfer_compaction_seconds;
                    while (pending_gpu_batches.size() < gpu_inflight_batches &&
                           submit_next_async_gpu_batch()) {
                    }
                } else {
                    const auto gpu_t0 = std::chrono::high_resolution_clock::now();
                    gpu_backend.filter_multi_into(
                        batch_start,
                        this_batch,
                        gpu_prefilter_plan.regions,
                        gpu_prefilter_plan.constraints,
                        hit_seeds
                    );
                    batch_gpu_seconds =
                        std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - gpu_t0).count();
                    batch_gpu_kernel_seconds = batch_gpu_seconds;
                }
                total_gpu_kernel_seconds += batch_gpu_kernel_seconds;
                total_gpu_transfer_compaction_seconds += batch_gpu_transfer_compaction_seconds;
                total_gpu_seconds += batch_gpu_seconds;
            } else {
                hit_seeds.resize(static_cast<size_t>(this_batch));
                for (uint64_t i = 0; i < this_batch; ++i) {
                    hit_seeds[static_cast<size_t>(i)] = (batch_start + i) & JAVA_MASK;
                }
            }
            batch_stats.gpu_kernel_seconds = batch_gpu_kernel_seconds;
            batch_stats.gpu_transfer_compaction_seconds = batch_gpu_transfer_compaction_seconds;
            batch_stats.gpu_seconds = batch_gpu_seconds;
            if (use_gpu_stage_a) {
                total_gpu_hits += hit_seeds.size();
            }
            batch_stats.stage_a_survivor_count = static_cast<uint64_t>(hit_seeds.size());
            batch_stats.stage_a_reject_count = this_batch > hit_seeds.size() ? this_batch - hit_seeds.size() : 0U;
            total_stage_a_survivors += batch_stats.stage_a_survivor_count;
            stage_a_reject_total += batch_stats.stage_a_reject_count;
            uint32_t valid_count = 0;
            uint32_t mismatch_count = 0;
            uint32_t biome_reject_count = 0;
            uint32_t cap_pruned = 0;
            if (valid_out.size() < hit_seeds.size()) {
                valid_out.resize(hit_seeds.size());
            }
            if (args.scan_mode == "placement") {
                valid_count = static_cast<uint32_t>(std::min<size_t>(
                    hit_seeds.size(),
                    static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
                if (valid_count > 0U) {
                    std::copy_n(hit_seeds.data(), valid_count, valid_out.data());
                }
                batch_stats.stage_a5_survivor_count = valid_count;
                batch_stats.stage_b_structure_survivor_count = valid_count;
                batch_stats.stage_c_biome_survivor_count = valid_count;
            } else if (!hit_seeds.empty()) {
                if (gpu_stage_a_final_filter && use_gpu_stage_a && !args.shadow_compare) {
                    valid_count = static_cast<uint32_t>(hit_seeds.size());
                    std::copy_n(hit_seeds.data(), valid_count, valid_out.data());
                    batch_stats.stage_a5_survivor_count = valid_count;
                    batch_stats.stage_b_structure_survivor_count = valid_count;
                    batch_stats.stage_c_biome_survivor_count = valid_count;
                } else {
                    const uint32_t effective_workers = choose_effective_validation_workers(
                        static_cast<uint32_t>(strict_workers),
                        hit_seeds.size()
                    );
                    const auto strict_t0 = std::chrono::high_resolution_clock::now();
                    if (args.shadow_compare) {
                    if (!query_plan.compiled_ready()) {
                        throw std::runtime_error("shadow-compare requires a compiled query plan.");
                    }
                    std::vector<uint64_t> compiled_valid(hit_seeds.size());
                    std::vector<uint64_t> legacy_valid(hit_seeds.size());
                    uint32_t compiled_valid_count = 0U;
                    uint32_t compiled_mismatch_count = 0U;
                    uint32_t compiled_biome_reject_count = 0U;
                    uint32_t compiled_cap_pruned = 0U;
                    uint32_t legacy_valid_count = 0U;
                    uint32_t legacy_mismatch_count = 0U;
                    uint32_t legacy_biome_reject_count = 0U;
                    uint32_t legacy_cap_pruned = 0U;

                    const int compiled_rc = query_plan.validate_batch_compiled(
                        hit_seeds.data(),
                        static_cast<uint32_t>(hit_seeds.size()),
                        effective_workers,
                        compiled_valid.data(),
                        &compiled_valid_count,
                        &compiled_mismatch_count,
                        &compiled_biome_reject_count,
                        &compiled_cap_pruned
                    );
                    const int legacy_rc = query_plan.validate_batch_legacy(
                        hit_seeds.data(),
                        static_cast<uint32_t>(hit_seeds.size()),
                        effective_workers,
                        legacy_valid.data(),
                        &legacy_valid_count,
                        &legacy_mismatch_count,
                        &legacy_biome_reject_count,
                        &legacy_cap_pruned
                    );
                    if (compiled_rc != legacy_rc) {
                        std::ostringstream oss;
                        oss << "shadow-compare batch rc mismatch: compiled=" << fmt_i64(compiled_rc)
                            << ", legacy=" << fmt_i64(legacy_rc) << ", batch_start=" << fmt_seed(batch_start)
                            << ", batch_count=" << fmt_u64(this_batch);
                        throw std::runtime_error(oss.str());
                    }
                    if (compiled_valid_count != legacy_valid_count ||
                        compiled_mismatch_count != legacy_mismatch_count ||
                        compiled_biome_reject_count != legacy_biome_reject_count ||
                        compiled_cap_pruned != legacy_cap_pruned ||
                        !std::equal(compiled_valid.begin(), compiled_valid.begin() + compiled_valid_count, legacy_valid.begin())) {
                        std::ostringstream oss;
                        oss << "shadow-compare validation mismatch: batch_start=" << fmt_seed(batch_start)
                            << ", batch_count=" << fmt_u64(this_batch) << ", compiled_valid="
                            << fmt_u64(compiled_valid_count) << ", legacy_valid=" << fmt_u64(legacy_valid_count)
                            << ", compiled_mismatch=" << fmt_u64(compiled_mismatch_count)
                            << ", legacy_mismatch=" << fmt_u64(legacy_mismatch_count)
                            << ", compiled_biome_reject=" << fmt_u64(compiled_biome_reject_count)
                            << ", legacy_biome_reject=" << fmt_u64(legacy_biome_reject_count)
                            << ", compiled_cap_pruned=" << fmt_u64(compiled_cap_pruned)
                            << ", legacy_cap_pruned=" << fmt_u64(legacy_cap_pruned);
                        throw std::runtime_error(oss.str());
                    }

                    std::unordered_set<uint64_t> detail_union;
                    detail_union.reserve(static_cast<size_t>(compiled_valid_count + legacy_valid_count));
                    for (uint32_t i = 0; i < compiled_valid_count; ++i) {
                        detail_union.insert(compiled_valid[i]);
                    }
                    for (uint32_t i = 0; i < legacy_valid_count; ++i) {
                        detail_union.insert(legacy_valid[i]);
                    }
                    const uint32_t match_cap = initial_query_match_cap(query_plan.constraints);
                    const uint32_t biome_cap = query_plan.biome_filters.empty()
                                                   ? 0U
                                                   : std::max<uint32_t>(1U, static_cast<uint32_t>(query_plan.biome_filters.size()));
                    for (uint64_t seed : detail_union) {
                        const DetailSnapshot compiled_detail =
                            collect_detail_snapshot_compiled(query_plan, seed, true, match_cap, biome_cap);
                        const DetailSnapshot legacy_detail =
                            collect_detail_snapshot_legacy(query_plan, seed, true, match_cap, biome_cap);
                        if (!snapshots_equal(compiled_detail, legacy_detail)) {
                            std::ostringstream oss;
                            oss << "shadow-compare detail mismatch: seed=" << fmt_seed(seed)
                                << ", batch_start=" << fmt_seed(batch_start)
                                << ", batch_count=" << fmt_u64(this_batch)
                                << ", compiled_rc=" << fmt_i64(compiled_detail.rc)
                                << ", legacy_rc=" << fmt_i64(legacy_detail.rc)
                                << ", compiled_matches=" << fmt_size(compiled_detail.matches.size())
                                << ", legacy_matches=" << fmt_size(legacy_detail.matches.size())
                                << ", compiled_biomes=" << fmt_size(compiled_detail.biomes.size())
                                << ", legacy_biomes=" << fmt_size(legacy_detail.biomes.size());
                            throw std::runtime_error(oss.str());
                        }
                    }

                    std::copy_n(compiled_valid.data(), compiled_valid_count, valid_out.data());
                    valid_count = compiled_valid_count;
                    mismatch_count = compiled_mismatch_count;
                    biome_reject_count = compiled_biome_reject_count;
                    cap_pruned = compiled_cap_pruned;
                    if (query_plan.compiled_ready() && scanner_core_query_plan_api().has_compiled_batch_stats_api()) {
                        if (scanner_core_query_plan_api().get_compiled_batch_stats(
                                query_plan.compiled_plan,
                                &batch_stats.core_stats) == 0) {
                            batch_stats.core_stats_available = true;
                            last_core_stats = batch_stats.core_stats;
                            last_core_stats_available = true;
                        }
                    }
                    } else {
                        const auto strict_t0_impl = std::chrono::high_resolution_clock::now();
                        const int rc = query_plan.validate_batch(
                            hit_seeds.data(),
                            static_cast<uint32_t>(hit_seeds.size()),
                            effective_workers,
                            valid_out.data(),
                            &valid_count,
                            &mismatch_count,
                            &biome_reject_count,
                            &cap_pruned
                        );
                        if (rc != 0) {
                            std::ostringstream oss;
                            oss << "scanner_core_validate_query_batch failed with code " << fmt_i64(rc);
                            if (rc == -2) {
                                oss << " (cubiomes lib not loaded)";
                            } else if (rc == -3) {
                                oss << " (cubiomes direct API unavailable)";
                            }
                            throw std::runtime_error(oss.str());
                        }
                        if (query_plan.compiled_ready() && scanner_core_query_plan_api().has_compiled_batch_stats_api()) {
                            if (scanner_core_query_plan_api().get_compiled_batch_stats(
                                    query_plan.compiled_plan,
                                    &batch_stats.core_stats) == 0) {
                                batch_stats.core_stats_available = true;
                                last_core_stats = batch_stats.core_stats;
                                last_core_stats_available = true;
                            }
                        }
                        (void)strict_t0_impl;
                    }
                    total_strict_seconds +=
                        std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - strict_t0).count();
                }
            }
            if (batch_stats.core_stats_available) {
                batch_stats.stage_a5_survivor_count = batch_stats.core_stats.stage_a5_survivor_count;
                batch_stats.stage_a5_reject_count = batch_stats.core_stats.stage_a5_reject_count;
                batch_stats.stage_b_structure_survivor_count = batch_stats.core_stats.stage_b_structure_survivor_count;
                batch_stats.stage_c_biome_survivor_count = batch_stats.core_stats.stage_c_biome_survivor_count;
                batch_stats.stage_b_exact_reject_count = batch_stats.core_stats.stage_b_exact_reject_count;
                batch_stats.stage_c_biome_reject_count = batch_stats.core_stats.stage_c_biome_reject_count;
                batch_stats.cpu_stage_a5_seconds = batch_stats.core_stats.cpu_stage_a5_seconds;
                batch_stats.cpu_stage_b_seconds = batch_stats.core_stats.cpu_stage_b_seconds;
                batch_stats.cpu_stage_c_seconds = batch_stats.core_stats.cpu_stage_c_seconds;
            } else {
                batch_stats.stage_b_structure_survivor_count =
                    static_cast<uint64_t>(valid_count) + static_cast<uint64_t>(biome_reject_count);
                batch_stats.stage_c_biome_survivor_count = static_cast<uint64_t>(valid_count);
                batch_stats.stage_b_exact_reject_count = static_cast<uint64_t>(mismatch_count);
                batch_stats.stage_c_biome_reject_count = static_cast<uint64_t>(biome_reject_count);
            }
            total_stage_b_structure_survivors += batch_stats.stage_b_structure_survivor_count;
            total_stage_c_biome_survivors += batch_stats.stage_c_biome_survivor_count;
            stage_b_exact_reject_total += batch_stats.stage_b_exact_reject_count;
            stage_c_biome_reject_total += batch_stats.stage_c_biome_reject_count;

            if (java_cubiomes_validation_enabled && valid_count > 0U) {
                if (java_worldgen_out.size() < valid_count) {
                    java_worldgen_out.resize(valid_count);
                }
                uint32_t java_valid_count = 0U;
                uint32_t java_reject_count = 0U;
                const auto java_t0 = std::chrono::high_resolution_clock::now();
                for (uint32_t i = 0; i < valid_count; ++i) {
                    if (validate_java_structure_matches_for_seed(
                            valid_out[i],
                            prepared_java_structure_filters,
                            loot_version_token,
                            query_plan,
                            loot_client,
                            java_worldgen_match_buffer,
                            java_worldgen_skip_cache)) {
                        java_worldgen_out[java_valid_count++] = valid_out[i];
                    } else {
                        ++java_reject_count;
                    }
                }
                total_java_worldgen_seconds +=
                    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - java_t0).count();
                if (java_valid_count > 0U) {
                    std::copy_n(java_worldgen_out.data(), java_valid_count, valid_out.data());
                }
                valid_count = java_valid_count;
                java_worldgen_rejected += java_reject_count;
            }

            if (structure_settings_enabled && valid_count > 0U) {
                if (structure_settings_out.size() < valid_count) {
                    structure_settings_out.resize(valid_count);
                }
                const auto settings_t0 = std::chrono::high_resolution_clock::now();
                const uint32_t settings_valid_count = validate_structure_settings_for_seed_batch(
                    valid_out.data(),
                    valid_count,
                    prepared_structure_settings_filters,
                    loot_version_token,
                    query_plan,
                    loot_client,
                    structure_settings_match_buffer,
                    structure_settings_requests,
                    structure_settings_request_indices,
                    structure_settings_results,
                    structure_settings_current,
                    structure_settings_next,
                    structure_settings_out.data());
                total_java_worldgen_seconds +=
                    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - settings_t0).count();
                if (settings_valid_count > 0U) {
                    std::copy_n(structure_settings_out.data(), settings_valid_count, valid_out.data());
                }
                java_worldgen_rejected += valid_count - settings_valid_count;
                valid_count = settings_valid_count;
            }

            if (sculpt_enabled && valid_count > 0U) {
                if (sculpt_out.size() < valid_count) {
                    sculpt_out.resize(valid_count);
                }
                uint32_t sculpt_valid_count = 0U;
                uint32_t sculpt_reject_count = 0U;
                const uint32_t sculpt_workers = choose_effective_validation_workers(
                    static_cast<uint32_t>(strict_workers),
                    valid_count
                );
                const auto sculpt_t0 = std::chrono::high_resolution_clock::now();
                const int sculpt_rc = scanner_core_filter_sculpt_batch(
                    valid_out.data(),
                    valid_count,
                    &native_sculpt,
                    native_sculpt_allowed.data(),
                    static_cast<uint32_t>(native_sculpt_allowed.size()),
                    cubi_mc_version,
                    sculpt_workers,
                    sculpt_out.data(),
                    &sculpt_valid_count,
                    &sculpt_reject_count
                );
                if (sculpt_rc != 0) {
                    std::ostringstream oss;
                    oss << "scanner_core_filter_sculpt_batch failed with code " << fmt_i64(sculpt_rc);
                    throw std::runtime_error(oss.str());
                }
                total_sculpt_seconds +=
                    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - sculpt_t0).count();
                if (sculpt_valid_count > 0U) {
                    std::copy_n(sculpt_out.data(), sculpt_valid_count, valid_out.data());
                }
                valid_count = sculpt_valid_count;
                sculpt_rejected += sculpt_reject_count;
            }
            if (loot_enabled && valid_count > 0U) {
                if (loot_out.size() < valid_count) {
                    loot_out.resize(valid_count);
                }
                uint32_t loot_valid_count = 0U;
                uint32_t loot_reject_count = 0U;
                const auto loot_t0 = std::chrono::high_resolution_clock::now();
                for (uint32_t i = 0; i < valid_count; ++i) {
                    if (validate_loot_filters_for_seed(
                            valid_out[i],
                            prepared_loot_filters,
                            loot_version_token,
                            query_plan,
                            loot_client,
                            loot_match_buffer,
                            loot_roll_cache)) {
                        loot_out[loot_valid_count++] = valid_out[i];
                    } else {
                        ++loot_reject_count;
                    }
                }
                total_loot_seconds +=
                    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - loot_t0).count();
                if (loot_valid_count > 0U) {
                    std::copy_n(loot_out.data(), loot_valid_count, valid_out.data());
                }
                valid_count = loot_valid_count;
                loot_rejected += loot_reject_count;
            }

            batch_stats.final_valid_count = valid_count;
            batch_stats.mismatches = mismatch_count;
            batch_stats.biome_rejected = biome_reject_count;
            batch_stats.sculpt_rejected = 0U;
            batch_stats.loot_rejected = 0U;
            batch_stats.cap_pruned_total = cap_pruned;
            total_cpu_verified += valid_count;
            total_stage_a5_survivors += batch_stats.stage_a5_survivor_count;
            stage_a5_reject_total += batch_stats.stage_a5_reject_count;
            mismatches += mismatch_count;
            biome_rejected += biome_reject_count;
            cap_pruned_total += cap_pruned;
            batch_stats.total_seconds = batch_stats.gpu_seconds + batch_stats.cpu_stage_a5_seconds +
                                        batch_stats.cpu_stage_b_seconds + batch_stats.cpu_stage_c_seconds;

            if (allow_runtime_auto_route && use_gpu_stage_a && batch_stats.input_seed_count > 0U) {
                const double actual_stage_a_pass_rate =
                    static_cast<double>(batch_stats.stage_a_survivor_count) /
                    static_cast<double>(batch_stats.input_seed_count);
                if (batch_idx == 1U && stage_a_route.route_decision == "single_driver" &&
                    !stage_a_route.best_multi_drivers.empty() && actual_stage_a_pass_rate > 0.01) {
                    stage_a_route.chosen_drivers = stage_a_route.best_multi_drivers;
                    stage_a_route.measured_selected_pass_rate = stage_a_route.measured_multi_driver_pass_rate;
                    stage_a_route.route_decision = "multi_driver";
                    stage_a_route.route_reason = "adaptive_high_single_pass_rate";
                    gpu_prefilter_plan = stage_a_route.multi_gpu_plan;
                    plan_summary.chosen_drivers = stage_a_route.chosen_drivers;
                    plan_summary.route_decision = stage_a_route.route_decision;
                    plan_summary.route_reason = stage_a_route.route_reason;
                    plan_summary.measured_selected_pass_rate = stage_a_route.measured_selected_pass_rate;
                } else if (stage_a_route.route_decision == "multi_driver" && actual_stage_a_pass_rate > 0.005) {
                    use_gpu_stage_a = false;
                    gpu_prefilter_plan = GpuPrefilterPlan{};
                    stage_a_route.gpu_enabled = false;
                    stage_a_route.route_decision = "cpu_stage_a5_heavy";
                    stage_a_route.route_reason = "adaptive_high_multi_pass_rate";
                    plan_summary.gpu_stage_a_enabled = false;
                    plan_summary.route_decision = stage_a_route.route_decision;
                    plan_summary.route_reason = stage_a_route.route_reason;
                }

                if (batch_stats.gpu_seconds > batch_stats.cpu_stage_b_seconds && actual_stage_a_pass_rate > 0.01) {
                    ++gpu_hurt_streak;
                } else {
                    gpu_hurt_streak = 0U;
                }
                if (gpu_hurt_streak >= 2U) {
                    use_gpu_stage_a = false;
                    gpu_prefilter_plan = GpuPrefilterPlan{};
                    stage_a_route.gpu_enabled = false;
                    stage_a_route.route_decision = "gpu_off";
                    stage_a_route.route_reason = "adaptive_gpu_cost_exceeds_saved_cpu";
                    plan_summary.gpu_stage_a_enabled = false;
                    plan_summary.route_decision = stage_a_route.route_decision;
                    plan_summary.route_reason = stage_a_route.route_reason;
                }
            }

            if (valid_count > 0 && static_cast<int>(printed_seeds.size()) < args.max_print) {
                const size_t need = static_cast<size_t>(args.max_print - static_cast<int>(printed_seeds.size()));
                const size_t add_n = std::min<size_t>(need, static_cast<size_t>(valid_count));
                for (size_t i = 0; i < add_n; ++i) {
                    const uint64_t seed = valid_out[i];
                    printed_seeds.push_back(seed);
                    if (args.stream_seeds) {
                        emit_seed_found_line(seed, args);
                    }
                }
            }
            if (valid_count > 0U && verified_txt) {
                for (uint32_t i = 0; i < valid_count; ++i) {
                    write_seed_txt_line(verified_txt, valid_out[i], args);
                }
            }

            const uint64_t processed = args.count - (remaining - this_batch);
            const auto now = std::chrono::high_resolution_clock::now();
            const bool emit_progress =
                args.progress_interval <= 0.0 ||
                std::chrono::duration<double>(now - last_progress).count() >= args.progress_interval ||
                processed >= args.count;
            if (emit_progress) {
                last_progress = now;
                const double elapsed = std::max(1e-9, std::chrono::duration<double>(now - t0).count());
                const double rate = static_cast<double>(processed) / elapsed;
                const double pct = (static_cast<double>(processed) / static_cast<double>(args.count)) * 100.0;
                if (terminal_mode.load()) {
                    std::cout << "Batch " << fmt_u64(batch_idx) << ": processed=" << fmt_u64(processed) << "/"
                              << fmt_u64(args.count) << " (" << fmt_double(pct, 2) << "%), rate="
                              << fmt_double(rate, 0) << " seeds/sec, gpu_hits=" << fmt_u64(total_gpu_hits)
                              << ", cpu_hits=" << fmt_u64(total_cpu_verified) << ", mismatches="
                              << fmt_u64(mismatches) << ", biome_rejected=" << fmt_u64(biome_rejected)
                              << ", sculpt_rejected=" << fmt_u64(sculpt_rejected)
                              << ", java_worldgen_rejected=" << fmt_u64(java_worldgen_rejected)
                              << ", loot_rejected=" << fmt_u64(loot_rejected)
                              << ", cap_pruned=" << fmt_u64(cap_pruned_total) << "\n";
                }
            }
            emit_ui_progress(processed, false);

            batch_start += this_batch;
            remaining -= this_batch;
        }
        }
        if (!final_progress_emitted) {
            emit_ui_progress(args.count, true);
        }

        const double elapsed = std::max(1e-9, std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count());
        const double rate = static_cast<double>(args.count) / elapsed;
        const double rate_per_worker = rate / static_cast<double>(std::max(1, strict_workers));

        if (terminal_mode.load()) {
            std::cout << "\nDone.\n";
            std::cout << "Elapsed: " << fmt_double(elapsed, 2) << "s\n";
            std::cout << "Throughput: " << fmt_double(rate, 0) << " seeds/sec\n";
            std::cout << "Throughput per worker: " << fmt_double(rate_per_worker, 0) << " seeds/sec\n";
            std::cout << "GPU hits: " << fmt_u64(total_gpu_hits) << "\n";
            std::cout << "CPU hits (verified): " << fmt_u64(total_cpu_verified) << "\n";
            std::cout << "GPU/CPU mismatches: " << fmt_u64(mismatches) << "\n";
            std::cout << "Rejected by biome filter: " << fmt_u64(biome_rejected) << "\n";
            std::cout << "Rejected by Java worldgen: " << fmt_u64(java_worldgen_rejected) << "\n";
            std::cout << "Rejected by sculpt: " << fmt_u64(sculpt_rejected) << "\n";
            std::cout << "Rejected by loot: " << fmt_u64(loot_rejected) << "\n";
            std::cout << "Cap-pruned candidates: " << fmt_u64(cap_pruned_total) << "\n";
        }
        const double stage_total = total_gpu_seconds + total_strict_seconds + total_java_worldgen_seconds +
                                   total_sculpt_seconds + total_loot_seconds;
        if (terminal_mode.load() && stage_total > 0.0) {
            const double gpu_pct = (total_gpu_seconds / stage_total) * 100.0;
            const double strict_pct = (total_strict_seconds / stage_total) * 100.0;
            const double java_pct = (total_java_worldgen_seconds / stage_total) * 100.0;
            const double sculpt_pct = (total_sculpt_seconds / stage_total) * 100.0;
            const double loot_pct = (total_loot_seconds / stage_total) * 100.0;
            std::cout << "Stage timings: gpu=" << fmt_double(total_gpu_seconds, 3) << "s (" << fmt_double(gpu_pct, 1)
                      << "%), strict=" << fmt_double(total_strict_seconds, 3) << "s (" << fmt_double(strict_pct, 1)
                      << "%), java_worldgen=" << fmt_double(total_java_worldgen_seconds, 3) << "s ("
                      << fmt_double(java_pct, 1)
                      << "%), sculpt=" << fmt_double(total_sculpt_seconds, 3) << "s (" << fmt_double(sculpt_pct, 1)
                      << "%), loot=" << fmt_double(total_loot_seconds, 3) << "s (" << fmt_double(loot_pct, 1)
                      << "%)\n";
        }
        if (args.plan_diagnostics || args.shadow_compare) {
            PlanDiagnosticsSummary final_summary = plan_summary;
            final_summary.observed_stage_a_pass_rate =
                args.count == 0 ? 0.0 : static_cast<double>(total_stage_a_survivors) / static_cast<double>(args.count);
            final_summary.observed_stage_b_reject_rate =
                total_stage_a_survivors == 0
                    ? 0.0
                    : static_cast<double>(stage_b_exact_reject_total) /
                          static_cast<double>(std::max<uint64_t>(1U, total_stage_a_survivors));
            RunBatchStats runtime_summary{};
            runtime_summary.input_seed_count = args.count;
            runtime_summary.stage_a_survivor_count = total_stage_a_survivors;
            runtime_summary.stage_a5_survivor_count = total_stage_a5_survivors;
            runtime_summary.stage_b_structure_survivor_count = total_stage_b_structure_survivors;
            runtime_summary.stage_c_biome_survivor_count = total_stage_c_biome_survivors;
            runtime_summary.final_valid_count = total_cpu_verified;
            runtime_summary.stage_a_reject_count = stage_a_reject_total;
            runtime_summary.stage_a5_reject_count = stage_a5_reject_total;
            runtime_summary.stage_b_exact_reject_count = stage_b_exact_reject_total;
            runtime_summary.stage_c_biome_reject_count = stage_c_biome_reject_total;
            runtime_summary.mismatches = mismatches;
            runtime_summary.biome_rejected = biome_rejected;
            runtime_summary.sculpt_rejected = sculpt_rejected;
            runtime_summary.loot_rejected = loot_rejected;
            runtime_summary.cap_pruned_total = cap_pruned_total;
            runtime_summary.gpu_kernel_seconds = total_gpu_kernel_seconds;
            runtime_summary.gpu_transfer_compaction_seconds = total_gpu_transfer_compaction_seconds;
            runtime_summary.cpu_stage_a5_seconds = 0.0;
            runtime_summary.gpu_seconds = total_gpu_seconds;
            runtime_summary.cpu_stage_b_seconds = total_strict_seconds;
            runtime_summary.cpu_stage_c_seconds =
                total_java_worldgen_seconds + total_sculpt_seconds + total_loot_seconds;
            runtime_summary.total_seconds = elapsed;
            std::cout << "PLAN_DIAGNOSTICS_END "
                      << describe_plan_diagnostics_json(
                             final_summary,
                             last_core_stats_available ? &last_core_stats : nullptr,
                             &runtime_summary) << "\n";
        }

        if (printed_seeds.empty()) {
            if (terminal_mode.load()) {
                std::cout << "No valid seeds found in this range.\n";
            }
            return 0;
        }

        if (args.print_closest) {
            print_seed_detail_report(
                printed_seeds.front(),
                args,
                runtime,
                query_plan,
                prepared_loot_filters,
                loot_version_token,
                loot_enabled ? &loot_client : nullptr
            );
        }

        if (terminal_mode.load()) {
            std::cout << "\nFirst " << fmt_size(printed_seeds.size()) << " matching seeds:\n";
            for (uint64_t seed : printed_seeds) {
                const uint64_t lifted = lift_seed_to_64(seed, args.upper16);
                if (args.output_mode == "raw") {
                    std::cout << fmt_seed(seed) << "\n";
                } else if (args.output_mode == "lift64") {
                    std::cout << fmt_seed(lifted) << "\n";
                } else {
                    std::cout << "raw=" << fmt_seed(seed) << "  lift64=" << fmt_seed(lifted) << "\n";
                }
            }
        }
        return 0;
    } catch (const std::exception &exc) {
        std::cerr << "Error: " << exc.what() << "\n";
        return 1;
    }
}
