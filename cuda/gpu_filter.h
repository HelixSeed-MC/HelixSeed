#ifndef GPU_FILTER_H
#define GPU_FILTER_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(GPU_FILTER_BUILD_DLL)
#define GPU_FILTER_API __declspec(dllexport)
#else
#define GPU_FILTER_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define GPU_FILTER_API __attribute__((visibility("default")))
#else
#define GPU_FILTER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RegionTerm {
    int32_t base_block_x;
    int32_t base_block_z;
    uint64_t add_term_mod48;
    uint32_t bound;
    // Legacy bookkeeping field retained for ABI compatibility. The exact
    // Stage A GPU path does not use it.
    uint32_t constraint_index;
    uint32_t spread_type;
    // Legacy fast-prefilter hint retained for compatibility with older callers.
    // Exact structure-only GPU paths ignore it and always evaluate exactly.
    uint32_t use_fast_next_int;
} RegionTerm;

typedef struct ConstraintDesc {
    uint32_t region_start;
    uint32_t region_count;
    uint64_t radius_sq;
    int32_t anchor_x;
    int32_t anchor_z;
    // Legacy surrogate-filter fields retained for ABI compatibility. The exact
    // Stage A GPU path ignores gate-only behavior and evaluates structure math
    // only from the region/radius terms below.
    uint32_t gate_div;
    uint32_t gate_salt;
    uint32_t is_gate_only;
    uint32_t min_required;
    uint32_t quad_max_span;
} ConstraintDesc;

GPU_FILTER_API int gpu_is_available(void);
GPU_FILTER_API uint64_t gpu_total_mem(void);

// Legacy single-constraint helper retained for compatibility with older callers.
// Exact compiled-plan stage A should prefer gpu_filter_multi.
GPU_FILTER_API void gpu_filter(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    int region_count,
    uint64_t radius_sq,
    uint32_t gate_div,
    uint32_t gate_salt,
    uint64_t *output_buffer,
    uint32_t *hit_count);

// Exact low-48 structure-only multi-constraint filter. Gate-only surrogate
// entries are ignored; callers should provide structure descriptors only.
GPU_FILTER_API void gpu_filter_multi(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count);

// Checked variant of gpu_filter_multi. Returns 0 on success, negative on
// backend/setup/runtime failure. Callers that care about correctness should
// prefer this entrypoint so a GPU error cannot be mistaken for an empty hit set.
GPU_FILTER_API int gpu_filter_multi_checked(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count);

// ---------------------------------------------------------------------------
// Double-buffered async API (Patch 1).
//
// gpu_filter_double_buffer_available returns 1 when the backend supports the
// submit/collect pair below. Older DLLs without this symbol must be treated as
// not supporting async, and callers should fall back to gpu_filter_multi_checked.
//
// gpu_filter_async_max_slots returns the number of async slots accepted by
// gpu_filter_multi_submit/collect. Older DLLs without this symbol should be
// treated as supporting two slots when gpu_filter_double_buffer_available is 1.
//
// gpu_filter_multi_submit launches a Stage A prefilter batch on the requested
// slot (0 <= slot < gpu_filter_async_max_slots()) and returns without waiting
// for the GPU. The slot must not already be in flight. Inputs are copied
// internally, so the caller may free or modify the buffers as soon as submit
// returns.
//
// gpu_filter_multi_collect blocks until the slot's GPU work and D2H transfers
// are done, copies the survivors into out_buffer (up to out_capacity entries),
// returns the actual hit count, and reports per-batch timings via the optional
// output pointers (set to nullptr if not needed).
//
// Both functions return 0 on success and the same negative status codes as
// gpu_filter_multi_checked on failure.
GPU_FILTER_API int gpu_filter_double_buffer_available(void);
GPU_FILTER_API uint32_t gpu_filter_async_max_slots(void);

GPU_FILTER_API int gpu_filter_multi_submit(
    uint32_t slot_id,
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count);

GPU_FILTER_API int gpu_filter_multi_collect(
    uint32_t slot_id,
    uint64_t *out_buffer,
    uint64_t out_capacity,
    uint32_t *out_hit_count,
    double *out_kernel_seconds,
    double *out_transfer_seconds);

#ifdef __cplusplus
}
#endif

#endif
