#include "gpu_filter.h"

extern "C" int gpu_is_available_impl(void);
extern "C" uint64_t gpu_total_mem_impl(void);
extern "C" void gpu_filter_impl(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    int region_count,
    uint64_t radius_sq,
    uint32_t gate_div,
    uint32_t gate_salt,
    uint64_t *output_buffer,
    uint32_t *hit_count);
extern "C" void gpu_filter_multi_impl(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count);
extern "C" int gpu_filter_multi_checked_impl(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count);
extern "C" int gpu_filter_double_buffer_available_impl(void);
extern "C" int gpu_filter_multi_submit_impl(
    uint32_t slot_id,
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count);
extern "C" int gpu_filter_multi_collect_impl(
    uint32_t slot_id,
    uint64_t *out_buffer,
    uint64_t out_capacity,
    uint32_t *out_hit_count,
    double *out_kernel_seconds,
    double *out_transfer_seconds);

extern "C" GPU_FILTER_API int gpu_is_available(void) {
    return gpu_is_available_impl();
}

extern "C" GPU_FILTER_API uint64_t gpu_total_mem(void) {
    return gpu_total_mem_impl();
}

extern "C" GPU_FILTER_API void gpu_filter(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    int region_count,
    uint64_t radius_sq,
    uint32_t gate_div,
    uint32_t gate_salt,
    uint64_t *output_buffer,
    uint32_t *hit_count) {
    gpu_filter_impl(
        start_seed,
        count,
        regions,
        region_count,
        radius_sq,
        gate_div,
        gate_salt,
        output_buffer,
        hit_count);
}

extern "C" GPU_FILTER_API void gpu_filter_multi(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count) {
    gpu_filter_multi_impl(
        start_seed,
        count,
        regions,
        region_count,
        constraints,
        constraint_count,
        output_buffer,
        hit_count);
}

extern "C" GPU_FILTER_API int gpu_filter_multi_checked(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count) {
    return gpu_filter_multi_checked_impl(
        start_seed,
        count,
        regions,
        region_count,
        constraints,
        constraint_count,
        output_buffer,
        hit_count);
}

extern "C" GPU_FILTER_API int gpu_filter_double_buffer_available(void) {
    return gpu_filter_double_buffer_available_impl();
}

extern "C" GPU_FILTER_API int gpu_filter_multi_submit(
    uint32_t slot_id,
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count) {
    return gpu_filter_multi_submit_impl(
        slot_id,
        start_seed,
        count,
        regions,
        region_count,
        constraints,
        constraint_count);
}

extern "C" GPU_FILTER_API int gpu_filter_multi_collect(
    uint32_t slot_id,
    uint64_t *out_buffer,
    uint64_t out_capacity,
    uint32_t *out_hit_count,
    double *out_kernel_seconds,
    double *out_transfer_seconds) {
    return gpu_filter_multi_collect_impl(
        slot_id,
        out_buffer,
        out_capacity,
        out_hit_count,
        out_kernel_seconds,
        out_transfer_seconds);
}
