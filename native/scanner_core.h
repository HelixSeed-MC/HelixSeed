#ifndef SCANNER_CORE_H
#define SCANNER_CORE_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(SCANNER_CORE_BUILD_DLL)
#define SCANNER_CORE_API __declspec(dllexport)
#else
#define SCANNER_CORE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define SCANNER_CORE_API __attribute__((visibility("default")))
#else
#define SCANNER_CORE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SCAN_SPREAD_LINEAR = 0,
    SCAN_SPREAD_TRIANGULAR = 1,
};

enum {
    SCAN_ANCHOR_ORIGIN = 0,
    SCAN_ANCHOR_SPAWN = 1,
    SCAN_ANCHOR_DEP = 2,
    SCAN_ANCHOR_CLOSEST = 3,
    SCAN_ANCHOR_FIXED = 4,
};

enum {
    SCAN_SCULPT_DISK = 0,
    SCAN_SCULPT_RING = 1,
    SCAN_SCULPT_GRID = 2,
    SCAN_SCULPT_CROSS = 3,
};

enum {
    SCAN_EDGE_OUTWARD = 0,
    SCAN_EDGE_INWARD = 1,
};

typedef struct NativeRegionTerm {
    int32_t rx;
    int32_t rz;
    int32_t base_block_x;
    int32_t base_block_z;
    uint64_t add_term_mod48;
    uint32_t bound;
    uint32_t spread_type;
    uint32_t structure_in_presets;
} NativeRegionTerm;

typedef struct NativeLegacyContextDesc {
    uint32_t region_start;
    uint32_t region_count;
    int32_t struct_id;
    uint32_t structure_in_presets;
} NativeLegacyContextDesc;

typedef struct NativeLegacyBiomeDesc {
    uint32_t enabled;
    uint32_t point_type; // origin|spawn|closest
    int32_t y;
    int32_t radius;
    uint32_t allowed_start;
    uint32_t allowed_count;
} NativeLegacyBiomeDesc;

typedef struct NativeQueryConstraintDesc {
    uint32_t region_start;
    uint32_t region_count;
    uint64_t candidate_radius_sq;
    uint32_t mode_strict;
    uint32_t anchor_type;  // origin|spawn|dep|fixed
    int32_t anchor_dep_index;
    uint32_t candidate_cap;
    uint32_t candidate_chunk_radius;
    uint32_t prefilter_supported;
    uint32_t region_spacing_chunks;
    uint32_t strict_region_spacing_hint;
    int32_t struct_id;
    uint32_t is_stronghold;
    uint32_t structure_in_presets;
    int32_t anchor_x;
    int32_t anchor_z;
    uint32_t min_required;
    uint32_t quad_max_span;
    uint32_t exact_attempt_prefilter;
    uint32_t attempt_bound;
    uint32_t attempt_spread_type;
    int32_t attempt_salt;
    uint32_t attempt_anchor_exact_stage0;
} NativeQueryConstraintDesc;

typedef struct NativeBiomeFilterDesc {
    uint32_t point_type;  // origin|spawn|dep|fixed
    int32_t point_dep_index;
    int32_t y;
    int32_t radius;
    uint32_t sample_step;
    uint32_t allowed_start;
    uint32_t allowed_count;
    int32_t point_x;
    int32_t point_z;
} NativeBiomeFilterDesc;

typedef struct NativeSculptDesc {
    uint32_t enabled;
    uint32_t pattern;      // disk|ring|grid|cross
    uint32_t edge_mode;    // outward|inward
    uint32_t center_type;  // origin|spawn|fixed
    int32_t center_x;
    int32_t center_z;
    int32_t y;
    int32_t radius;
    int32_t step;
    double dominance_threshold;
} NativeSculptDesc;

typedef struct NativeQueryMatchOut {
    uint32_t constraint_index;
    int32_t x;
    int32_t z;
    int32_t anchor_x;
    int32_t anchor_z;
    uint64_t dist2;
} NativeQueryMatchOut;

typedef struct NativeQueryBiomeOut {
    uint32_t filter_index;
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t radius;
    uint64_t dist2;
    int32_t biome_id;
} NativeQueryBiomeOut;

typedef struct ScannerCompiledQueryPlan ScannerCompiledQueryPlan;

typedef struct ScannerCompiledBatchStats {
    uint64_t input_seed_count;
    uint64_t stage_a_survivor_count;
    uint64_t stage_a5_survivor_count;
    uint64_t stage_b_structure_survivor_count;
    uint64_t stage_c_biome_survivor_count;
    uint64_t final_valid_count;
    uint64_t stage_a_reject_count;
    uint64_t stage_a5_reject_count;
    uint64_t stage_b_exact_reject_count;
    uint64_t stage_c_biome_reject_count;
    uint64_t setup_generator_calls;
    uint64_t apply_seed_calls;
    uint64_t get_structure_pos_calls;
    uint64_t is_viable_structure_pos_calls;
    uint64_t get_biome_at_calls;
    uint64_t get_spawn_calls;
    uint64_t estimate_spawn_calls;
    uint64_t init_first_stronghold_calls;
    uint64_t next_stronghold_calls;
    double cpu_stage_a5_seconds;
    double cpu_stage_b_seconds;
    double cpu_stage_c_seconds;
    uint32_t constraint_count;
    const uint64_t *per_constraint_stage_a_rejects;
    const uint64_t *per_constraint_stage_a5_rejects;
    const uint64_t *per_constraint_stage_b_exact_rejects;
} ScannerCompiledBatchStats;

SCANNER_CORE_API int scanner_core_is_available(void);
SCANNER_CORE_API int scanner_core_set_cubiomes_lib_path(const char *path_utf8);

SCANNER_CORE_API int scanner_core_validate_legacy_batch(
    const uint64_t *seeds,
    uint32_t seed_count,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeLegacyContextDesc *contexts,
    uint32_t context_count,
    uint64_t radius_sq,
    uint32_t strict_viability,
    int32_t cubi_mc_version,
    const NativeLegacyBiomeDesc *biome_desc,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count,
    uint32_t worker_count,
    uint64_t *valid_out,
    uint32_t *valid_count,
    uint32_t *mismatch_count,
    uint32_t *biome_reject_count);

SCANNER_CORE_API int scanner_core_validate_query_batch(
    const uint64_t *seeds,
    uint32_t seed_count,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count,
    int32_t cubi_mc_version,
    uint32_t worker_count,
    uint64_t *valid_out,
    uint32_t *valid_count,
    uint32_t *mismatch_count,
    uint32_t *biome_reject_count,
    uint32_t *cap_pruned_total);

SCANNER_CORE_API int scanner_core_validate_query_batch_legacy(
    const uint64_t *seeds,
    uint32_t seed_count,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count,
    int32_t cubi_mc_version,
    uint32_t worker_count,
    uint64_t *valid_out,
    uint32_t *valid_count,
    uint32_t *mismatch_count,
    uint32_t *biome_reject_count,
    uint32_t *cap_pruned_total);

SCANNER_CORE_API ScannerCompiledQueryPlan *scanner_core_compile_query_plan(
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count);

SCANNER_CORE_API void scanner_core_free_query_plan(ScannerCompiledQueryPlan *plan);

SCANNER_CORE_API int scanner_core_validate_query_batch_compiled(
    const ScannerCompiledQueryPlan *plan,
    const uint64_t *seeds,
    uint32_t seed_count,
    int32_t cubi_mc_version,
    uint32_t worker_count,
    uint64_t *valid_out,
    uint32_t *valid_count,
    uint32_t *mismatch_count,
    uint32_t *biome_reject_count,
    uint32_t *cap_pruned_total);

SCANNER_CORE_API int scanner_core_get_compiled_batch_stats(
    const ScannerCompiledQueryPlan *plan,
    ScannerCompiledBatchStats *stats);

SCANNER_CORE_API int scanner_core_collect_query_details(
    uint64_t seed,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count,
    int32_t cubi_mc_version,
    NativeQueryMatchOut *match_out,
    uint32_t match_cap,
    uint32_t *match_count,
    NativeQueryBiomeOut *biome_out,
    uint32_t biome_cap,
    uint32_t *biome_count,
    uint32_t *cap_pruned_total);

SCANNER_CORE_API int scanner_core_collect_query_details_legacy(
    uint64_t seed,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count,
    int32_t cubi_mc_version,
    NativeQueryMatchOut *match_out,
    uint32_t match_cap,
    uint32_t *match_count,
    NativeQueryBiomeOut *biome_out,
    uint32_t biome_cap,
    uint32_t *biome_count,
    uint32_t *cap_pruned_total);

SCANNER_CORE_API int scanner_core_collect_query_details_compiled(
    const ScannerCompiledQueryPlan *plan,
    uint64_t seed,
    int32_t cubi_mc_version,
    NativeQueryMatchOut *match_out,
    uint32_t match_cap,
    uint32_t *match_count,
    NativeQueryBiomeOut *biome_out,
    uint32_t biome_cap,
    uint32_t *biome_count,
    uint32_t *cap_pruned_total);

SCANNER_CORE_API int scanner_core_filter_sculpt_batch(
    const uint64_t *seeds,
    uint32_t seed_count,
    const NativeSculptDesc *sculpt_desc,
    const int32_t *allowed_ids,
    uint32_t allowed_count,
    int32_t cubi_mc_version,
    uint32_t worker_count,
    uint64_t *valid_out,
    uint32_t *valid_count,
    uint32_t *reject_count);

#ifdef __cplusplus
}
#endif

#endif
