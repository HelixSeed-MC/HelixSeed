#if defined(_WIN32) && !defined(SCANNER_CORE_BUILD_DLL)
#define SCANNER_CORE_BUILD_DLL
#endif
#include "scanner_core.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct ScannerCompiledQueryPlan {
    void *impl = nullptr;
};

namespace {

#if defined(_MSC_VER)
#define SCANNER_FORCE_INLINE __forceinline
#else
#define SCANNER_FORCE_INLINE inline __attribute__((always_inline))
#endif

constexpr uint64_t JAVA_MULT = 25214903917ULL;
constexpr uint64_t JAVA_ADD = 11ULL;
constexpr uint64_t JAVA_MASK = ((1ULL << 48U) - 1ULL);
constexpr int64_t REGION_X_MULT = 341873128712LL;
constexpr int64_t REGION_Z_MULT = 132897987541LL;
constexpr uint32_t INT32_SIGN = 0x80000000U;
constexpr int32_t CHUNK_SIZE = 16;
constexpr int32_t DIM_NETHER = -1;
constexpr int32_t DIM_OVERWORLD = 0;
constexpr int32_t DIM_END = 1;
// Local mirrors of cubiomes MCVersion enum thresholds used by the fast spawn-area
// prefilter. Keep these aligned with tmp_cubiomes_src/biomes.h.
constexpr int32_t MC_VERSION_1_12 = 15;
constexpr int32_t MC_VERSION_1_13 = 16;
constexpr int32_t MC_VERSION_1_17 = 21;
constexpr int32_t SPAWN_AREA_PREFILTER_RADIUS_PRE118 = 288;
constexpr int32_t SPAWN_AREA_PREFILTER_RADIUS_118_PLUS = 96;
constexpr int32_t SPAWN_AREA_PREFILTER_STEP = 4;
constexpr uint32_t EARLY_FIXED_BIOME_MAX_SAMPLES = 128U;

struct CPos {
    int32_t x;
    int32_t z;
};

struct StrongholdIterNative {
    CPos pos;
    CPos nextapprox;
    int32_t index;
    int32_t ringnum;
    int32_t ringmax;
    int32_t ringidx;
    double angle;
    double dist;
    uint64_t rnds;
    int32_t mc;
};

using FnSetupGenerator = void (*)(void *, int, uint32_t);
using FnApplySeed = void (*)(void *, int, uint64_t);
using FnGetStructurePosDirect = int (*)(int, int, uint64_t, int, int, CPos *);
using FnIsViableStructurePosDirect = int (*)(int, void *, int, int, uint32_t);
using FnEstimateSpawnDirect = CPos (*)(const void *, uint64_t *);
using FnGetSpawnDirect = CPos (*)(void *);
using FnGetBiomeAtDirect = int (*)(void *, int, int, int, int);
using FnInitFirstStronghold = CPos (*)(StrongholdIterNative *, int, uint64_t);
using FnNextStronghold = int (*)(StrongholdIterNative *, const void *);

#if defined(_WIN32)
using ModuleHandle = HMODULE;
static void *lookup_symbol(ModuleHandle mod, const char *name) {
    return reinterpret_cast<void *>(GetProcAddress(mod, name));
}
#else
using ModuleHandle = void *;
static void *lookup_symbol(ModuleHandle mod, const char *name) {
    return dlsym(mod, name);
}
#endif

struct CubiApi {
    ModuleHandle module = nullptr;
    bool loaded = false;
    bool direct_ready = false;

    FnSetupGenerator setup_generator = nullptr;
    FnApplySeed apply_seed = nullptr;
    FnGetStructurePosDirect get_structure_pos_direct = nullptr;
    FnIsViableStructurePosDirect is_viable_structure_pos_direct = nullptr;
    FnEstimateSpawnDirect estimate_spawn_direct = nullptr;
    FnGetSpawnDirect get_spawn_direct = nullptr;
    FnGetBiomeAtDirect get_biome_at_direct = nullptr;
    FnInitFirstStronghold init_first_stronghold = nullptr;
    FnNextStronghold next_stronghold = nullptr;
};

static CubiApi g_cubi;
static std::mutex g_cubi_mu;

static bool bind_cubi_symbols(CubiApi &api) {
    if (api.module == nullptr) {
        api.loaded = false;
        api.direct_ready = false;
        return false;
    }

    api.setup_generator = reinterpret_cast<FnSetupGenerator>(lookup_symbol(api.module, "setupGenerator"));
    api.apply_seed = reinterpret_cast<FnApplySeed>(lookup_symbol(api.module, "applySeed"));
    api.get_structure_pos_direct =
        reinterpret_cast<FnGetStructurePosDirect>(lookup_symbol(api.module, "getStructurePos"));
    api.is_viable_structure_pos_direct =
        reinterpret_cast<FnIsViableStructurePosDirect>(lookup_symbol(api.module, "isViableStructurePos"));
    api.estimate_spawn_direct = reinterpret_cast<FnEstimateSpawnDirect>(lookup_symbol(api.module, "estimateSpawn"));
    api.get_spawn_direct = reinterpret_cast<FnGetSpawnDirect>(lookup_symbol(api.module, "getSpawn"));
    api.get_biome_at_direct = reinterpret_cast<FnGetBiomeAtDirect>(lookup_symbol(api.module, "getBiomeAt"));
    api.init_first_stronghold =
        reinterpret_cast<FnInitFirstStronghold>(lookup_symbol(api.module, "initFirstStronghold"));
    api.next_stronghold = reinterpret_cast<FnNextStronghold>(lookup_symbol(api.module, "nextStronghold"));

    api.loaded = true;
    api.direct_ready = api.setup_generator != nullptr && api.apply_seed != nullptr &&
                       api.get_structure_pos_direct != nullptr &&
                       api.is_viable_structure_pos_direct != nullptr && api.get_spawn_direct != nullptr &&
                       api.get_biome_at_direct != nullptr && api.init_first_stronghold != nullptr &&
                       api.next_stronghold != nullptr;
    return api.loaded;
}

static bool load_cubi_locked(const char *path_utf8) {
#if defined(_WIN32)
    ModuleHandle mod = nullptr;
    if (path_utf8 != nullptr && path_utf8[0] != '\0') {
        mod = LoadLibraryA(path_utf8);
    } else {
        mod = GetModuleHandleA("lib.dll");
    }
    if (mod == nullptr) {
        return false;
    }
    g_cubi.module = mod;
    return bind_cubi_symbols(g_cubi);
#else
    ModuleHandle mod = nullptr;
    if (path_utf8 != nullptr && path_utf8[0] != '\0') {
        mod = dlopen(path_utf8, RTLD_LAZY | RTLD_LOCAL);
    } else {
        mod = dlopen(nullptr, RTLD_LAZY | RTLD_LOCAL);
    }
    if (mod == nullptr) {
        return false;
    }
    g_cubi.module = mod;
    return bind_cubi_symbols(g_cubi);
#endif
}

struct ThreadCubiState {
    std::array<uint64_t, 16384> generator_storage{};
    void *generator_ptr = generator_storage.data();
    bool generator_ready = false;
    int generator_version = -1;
    int generator_dim = 0;
    uint64_t generator_seed = 0;
    struct CubiCallCounters *call_counters = nullptr;
};

struct CubiCallCounters {
    uint64_t setup_generator_calls = 0ULL;
    uint64_t apply_seed_calls = 0ULL;
    uint64_t get_structure_pos_calls = 0ULL;
    uint64_t is_viable_structure_pos_calls = 0ULL;
    uint64_t get_biome_at_calls = 0ULL;
    uint64_t get_spawn_calls = 0ULL;
    uint64_t estimate_spawn_calls = 0ULL;
    uint64_t init_first_stronghold_calls = 0ULL;
    uint64_t next_stronghold_calls = 0ULL;
};

static SCANNER_FORCE_INLINE void *ensure_generator(
    const CubiApi &api,
    ThreadCubiState &st,
    int mc_version,
    uint64_t world_seed,
    int dim = DIM_OVERWORLD
) {
    if (!api.direct_ready) {
        return nullptr;
    }
    if (!st.generator_ready || st.generator_version != mc_version) {
        if (st.call_counters != nullptr) {
            ++st.call_counters->setup_generator_calls;
        }
        api.setup_generator(st.generator_ptr, mc_version, 0U);
        st.generator_ready = true;
        st.generator_version = mc_version;
        st.generator_dim = dim;
        st.generator_seed = ~world_seed;
    }
    if (st.generator_seed != world_seed || st.generator_dim != dim) {
        if (st.call_counters != nullptr) {
            ++st.call_counters->apply_seed_calls;
        }
        api.apply_seed(st.generator_ptr, dim, world_seed);
        st.generator_seed = world_seed;
        st.generator_dim = dim;
    }
    return st.generator_ptr;
}

static SCANNER_FORCE_INLINE uint64_t pack_xz_i32(int32_t x, int32_t z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32U) |
           static_cast<uint64_t>(static_cast<uint32_t>(z));
}

static SCANNER_FORCE_INLINE int32_t unpack_i32(uint32_t v) {
    return (v & 0x80000000U) ? static_cast<int32_t>(v - 0x100000000ULL) : static_cast<int32_t>(v);
}

static SCANNER_FORCE_INLINE int32_t unpack_x_from_packed(uint64_t packed) {
    return unpack_i32(static_cast<uint32_t>((packed >> 32U) & 0xFFFFFFFFULL));
}

static SCANNER_FORCE_INLINE int32_t unpack_z_from_packed(uint64_t packed) {
    return unpack_i32(static_cast<uint32_t>(packed & 0xFFFFFFFFULL));
}

static bool has_compact_group_within_span(
    const std::vector<uint64_t> &packed_points,
    uint32_t min_required,
    uint32_t max_span
) {
    if (packed_points.size() < static_cast<size_t>(std::max<uint32_t>(1U, min_required))) {
        return false;
    }
    if (min_required <= 1U || max_span == 0U) {
        return true;
    }

    struct XZ {
        int32_t x;
        int32_t z;
    };
    std::vector<XZ> points;
    points.reserve(packed_points.size());
    for (uint64_t p : packed_points) {
        points.push_back(XZ{unpack_x_from_packed(p), unpack_z_from_packed(p)});
    }
    std::sort(points.begin(), points.end(), [](const XZ &a, const XZ &b) {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        return a.z < b.z;
    });

    const int64_t span = static_cast<int64_t>(max_span);
    std::vector<int32_t> z_window;
    z_window.reserve(points.size());

    size_t right = 0;
    for (size_t left = 0; left < points.size(); ++left) {
        if (right < left) {
            right = left;
        }
        const int64_t max_x = static_cast<int64_t>(points[left].x) + span;
        while (right < points.size() && static_cast<int64_t>(points[right].x) <= max_x) {
            ++right;
        }
        if ((right - left) < static_cast<size_t>(min_required)) {
            continue;
        }

        z_window.clear();
        for (size_t i = left; i < right; ++i) {
            z_window.push_back(points[i].z);
        }
        std::sort(z_window.begin(), z_window.end());

        size_t z_hi = 0;
        for (size_t z_lo = 0; z_lo < z_window.size(); ++z_lo) {
            if (z_hi < z_lo) {
                z_hi = z_lo;
            }
            const int64_t max_z = static_cast<int64_t>(z_window[z_lo]) + span;
            while (z_hi < z_window.size() && static_cast<int64_t>(z_window[z_hi]) <= max_z) {
                ++z_hi;
            }
            if ((z_hi - z_lo) >= static_cast<size_t>(min_required)) {
                return true;
            }
        }
    }
    return false;
}

static SCANNER_FORCE_INLINE int32_t floor_div_i32(int32_t a, int32_t b) {
    int32_t q = a / b;
    int32_t r = a % b;
    if (r != 0 && ((r > 0) != (b > 0))) {
        --q;
    }
    return q;
}

static SCANNER_FORCE_INLINE uint32_t java_next_int_exact(uint64_t &state48, uint32_t bound) {
    if (bound == 0U) {
        return 0U;
    }
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

static SCANNER_FORCE_INLINE bool buried_treasure_frequency_pass(uint64_t seed, const NativeRegionTerm &region) {
    uint64_t state = ((seed + (region.add_term_mod48 & JAVA_MASK)) ^ JAVA_MULT) & JAVA_MASK;
    state = (state * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
    const uint32_t bits24 = static_cast<uint32_t>(state >> 24U);
    return bits24 < 167773U;
}

static SCANNER_FORCE_INLINE void structure_attempt_block_pos(
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

    uint32_t off_x = 0;
    uint32_t off_z = 0;
    if (region.spread_type == SCAN_SPREAD_TRIANGULAR) {
        const uint32_t x1 = java_next_int_exact(state, bound);
        const uint32_t x2 = java_next_int_exact(state, bound);
        const uint32_t z1 = java_next_int_exact(state, bound);
        const uint32_t z2 = java_next_int_exact(state, bound);
        off_x = (x1 + x2) / 2U;
        off_z = (z1 + z2) / 2U;
    } else {
        off_x = java_next_int_exact(state, bound);
        off_z = java_next_int_exact(state, bound);
    }

    out_x = region.base_block_x + static_cast<int32_t>(off_x * static_cast<uint32_t>(CHUNK_SIZE));
    out_z = region.base_block_z + static_cast<int32_t>(off_z * static_cast<uint32_t>(CHUNK_SIZE));
}

static SCANNER_FORCE_INLINE uint64_t dist2_i64(int32_t x0, int32_t z0, int32_t x1, int32_t z1) {
    const int64_t dx = static_cast<int64_t>(x0) - static_cast<int64_t>(x1);
    const int64_t dz = static_cast<int64_t>(z0) - static_cast<int64_t>(z1);
    return static_cast<uint64_t>(dx * dx + dz * dz);
}

static SCANNER_FORCE_INLINE bool structure_attempt_within_radius(
    uint64_t seed,
    const NativeRegionTerm &region,
    int32_t anchor_x,
    int32_t anchor_z,
    uint64_t radius_sq,
    int32_t *out_x = nullptr,
    int32_t *out_z = nullptr
) {
    if (region.spread_type == SCAN_SPREAD_BURIED_TREASURE && !buried_treasure_frequency_pass(seed, region)) {
        return false;
    }

    int32_t x = 0;
    int32_t z = 0;
    structure_attempt_block_pos(seed, region, x, z);
    if (out_x != nullptr) {
        *out_x = x;
    }
    if (out_z != nullptr) {
        *out_z = z;
    }
    return dist2_i64(x, z, anchor_x, anchor_z) <= radius_sq;
}

static SCANNER_FORCE_INLINE uint64_t query_attempt_add_term_mod48(int32_t rx, int32_t rz, int32_t salt) {
    const int64_t add_term =
        (static_cast<int64_t>(salt) + static_cast<int64_t>(rx) * REGION_X_MULT +
         static_cast<int64_t>(rz) * REGION_Z_MULT) &
        static_cast<int64_t>(JAVA_MASK);
    return static_cast<uint64_t>(add_term);
}

static SCANNER_FORCE_INLINE bool direct_structure_attempt_within_radius(
    uint64_t seed,
    const NativeQueryConstraintDesc &rc,
    int32_t rx,
    int32_t rz,
    int32_t anchor_x,
    int32_t anchor_z
) {
    if (rc.exact_attempt_prefilter == 0U) {
        return true;
    }

    const int32_t spacing = std::max<int32_t>(1, static_cast<int32_t>(rc.strict_region_spacing_hint));
    NativeRegionTerm attempt_region{};
    attempt_region.rx = rx;
    attempt_region.rz = rz;
    attempt_region.base_block_x = rx * spacing * CHUNK_SIZE;
    attempt_region.base_block_z = rz * spacing * CHUNK_SIZE;
    attempt_region.add_term_mod48 = query_attempt_add_term_mod48(rx, rz, rc.attempt_salt);
    attempt_region.bound = std::max<uint32_t>(1U, rc.attempt_bound);
    attempt_region.spread_type = rc.attempt_spread_type;
    return structure_attempt_within_radius(seed, attempt_region, anchor_x, anchor_z, rc.candidate_radius_sq);
}

static SCANNER_FORCE_INLINE bool structure_has_viability_check(int32_t struct_id) {
    // Cubiomes currently rejects isViableStructurePos for trail ruins.
    return struct_id >= 0 && struct_id != 23;
}

static SCANNER_FORCE_INLINE bool constraint_requires_exact_generator(const NativeQueryConstraintDesc &rc) {
    // Placement mode is intentionally structure-placement only. Strict mode,
    // strongholds, and spawn anchors still require Cubiomes-backed generation.
    return rc.is_stronghold != 0U || rc.anchor_type == SCAN_ANCHOR_SPAWN || rc.mode_strict != 0U;
}

static const char *debug_structure_name_from_id(int32_t struct_id) {
    switch (struct_id) {
    case 1: return "desert_pyramid";
    case 2: return "jungle_temple";
    case 3: return "swamp_hut";
    case 4: return "igloo";
    case 5: return "village";
    case 6: return "ocean_ruin";
    case 7: return "shipwreck";
    case 8: return "ocean_monument";
    case 9: return "woodland_mansion";
    case 10: return "pillager_outpost";
    case 11: return "ruined_portal";
    case 12: return "ruined_portal_nether";
    case 13: return "ancient_city";
    case 14: return "buried_treasure";
    case 15: return "mineshaft";
    case 16: return "desert_well";
    case 17: return "geode";
    case 18: return "fortress";
    case 19: return "bastion_remnant";
    case 20: return "end_city";
    case 23: return "trail_ruin";
    case 24: return "trial_chambers";
    case 25: return "nether_fossil";
    default: return "unknown";
    }
}

static bool prefilter_debug_enabled() {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = std::getenv("SCANNER_DEBUG_ATTEMPT_PREFILTER");
        enabled = (env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0) ? 1 : 0;
    }
    return enabled != 0;
}

static void debug_validate_structure_prefilter_rejection(
    const char *type_label,
    const char *path_label,
    const CubiApi &api,
    uint64_t seed,
    int mc_version,
    const NativeQueryConstraintDesc &rc,
    int32_t rx,
    int32_t rz,
    int32_t anchor_x,
    int32_t anchor_z,
    void *g_world
) {
    if (!prefilter_debug_enabled() || g_world == nullptr || rc.struct_id < 0) {
        return;
    }

    CPos pos{};
    if (!api.get_structure_pos_direct(rc.struct_id, mc_version, seed, rx, rz, &pos)) {
        return;
    }
    if (structure_has_viability_check(rc.struct_id) &&
        api.is_viable_structure_pos_direct(rc.struct_id, g_world, pos.x, pos.z, 0U) != 1) {
        return;
    }
    if (dist2_i64(pos.x, pos.z, anchor_x, anchor_z) > rc.candidate_radius_sq) {
        return;
    }

    std::fprintf(
        stderr,
        "[prefilter-debug] false_negative type=%s path=%s structure=%s(%d) seed=%llu region=(%d,%d) anchor=(%d,%d) pos=(%d,%d)\n",
        type_label,
        path_label,
        debug_structure_name_from_id(rc.struct_id),
        rc.struct_id,
        static_cast<unsigned long long>(seed),
        rx,
        rz,
        anchor_x,
        anchor_z,
        pos.x,
        pos.z
    );
}

static void debug_validate_attempt_prefilter_rejection(
    const char *path_label,
    const CubiApi &api,
    uint64_t seed,
    int mc_version,
    const NativeQueryConstraintDesc &rc,
    int32_t rx,
    int32_t rz,
    int32_t anchor_x,
    int32_t anchor_z,
    void *g_world
) {
    debug_validate_structure_prefilter_rejection(
        "attempt_radius",
        path_label,
        api,
        seed,
        mc_version,
        rc,
        rx,
        rz,
        anchor_x,
        anchor_z,
        g_world
    );
}

static void debug_validate_region_prune_rejection(
    const char *path_label,
    const CubiApi &api,
    uint64_t seed,
    int mc_version,
    const NativeQueryConstraintDesc &rc,
    int32_t rx,
    int32_t rz,
    int32_t anchor_x,
    int32_t anchor_z,
    void *g_world
) {
    debug_validate_structure_prefilter_rejection(
        "region_aabb",
        path_label,
        api,
        seed,
        mc_version,
        rc,
        rx,
        rz,
        anchor_x,
        anchor_z,
        g_world
    );
}

static void debug_log_biome_prefilter_false_negative(
    uint64_t seed,
    uint32_t filter_index,
    const NativeBiomeFilterDesc &desc,
    int32_t point_x,
    int32_t point_z
) {
    if (!prefilter_debug_enabled()) {
        return;
    }
    std::fprintf(
        stderr,
        "[prefilter-debug] false_negative type=independent_biome seed=%llu filter=%u point_type=%u anchor=(%d,%d) y=%d radius=%d\n",
        static_cast<unsigned long long>(seed),
        static_cast<unsigned int>(filter_index),
        static_cast<unsigned int>(desc.point_type),
        point_x,
        point_z,
        desc.y,
        desc.radius
    );
}

static void debug_log_spawn_area_prefilter_false_negative(
    uint64_t seed,
    uint32_t filter_index,
    const NativeBiomeFilterDesc &desc,
    int32_t sample_radius
) {
    if (!prefilter_debug_enabled()) {
        return;
    }
    std::fprintf(
        stderr,
        "[prefilter-debug] false_negative type=spawn_area seed=%llu filter=%u point_type=%u sample_radius=%d y=%d radius=%d\n",
        static_cast<unsigned long long>(seed),
        static_cast<unsigned int>(filter_index),
        static_cast<unsigned int>(desc.point_type),
        sample_radius,
        desc.y,
        desc.radius
    );
}

static SCANNER_FORCE_INLINE int32_t spawn_area_prefilter_search_radius(int mc_version) {
    if (mc_version <= MC_VERSION_1_12) {
        return 0;
    }
    if (mc_version <= MC_VERSION_1_17) {
        return SPAWN_AREA_PREFILTER_RADIUS_PRE118;
    }
    return SPAWN_AREA_PREFILTER_RADIUS_118_PLUS;
}

struct BiomeOffset {
    int32_t dx;
    int32_t dz;
    uint64_t d2;
};

struct SculptOffset {
    int32_t dx;
    int32_t dz;
    uint64_t d2;
};

static std::vector<BiomeOffset> build_biome_offsets(int32_t radius, int32_t step) {
    const int32_t r = std::max<int32_t>(0, radius);
    const int32_t s = std::max<int32_t>(1, step);

    std::vector<BiomeOffset> out;
    out.reserve(static_cast<size_t>((2 * r / s + 3) * (2 * r / s + 3)));
    out.push_back(BiomeOffset{0, 0, 0});

    if (r == 0) {
        return out;
    }

    const int64_t r2 = static_cast<int64_t>(r) * static_cast<int64_t>(r);
    for (int32_t dz = -r; dz <= r; dz += s) {
        for (int32_t dx = -r; dx <= r; dx += s) {
            const int64_t d2 = static_cast<int64_t>(dx) * static_cast<int64_t>(dx) +
                               static_cast<int64_t>(dz) * static_cast<int64_t>(dz);
            if (d2 == 0 || d2 > r2) {
                continue;
            }
            out.push_back(BiomeOffset{dx, dz, static_cast<uint64_t>(d2)});
        }
    }

    std::sort(out.begin(), out.end(), [](const BiomeOffset &a, const BiomeOffset &b) { return a.d2 < b.d2; });
    return out;
}

static std::mutex g_biome_offsets_mu;
static std::unordered_map<uint64_t, std::vector<BiomeOffset>> g_biome_offsets_cache;

static SCANNER_FORCE_INLINE uint64_t biome_offset_cache_key(int32_t radius, int32_t step) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(radius)) << 32U) |
           static_cast<uint64_t>(static_cast<uint32_t>(step));
}

static const std::vector<BiomeOffset> &build_biome_offsets_cached(int32_t radius, int32_t step) {
    const int32_t r = std::max<int32_t>(0, radius);
    const int32_t s = std::max<int32_t>(1, step);
    const uint64_t key = biome_offset_cache_key(r, s);

    {
        std::lock_guard<std::mutex> lock(g_biome_offsets_mu);
        const auto it = g_biome_offsets_cache.find(key);
        if (it != g_biome_offsets_cache.end()) {
            return it->second;
        }
    }

    std::vector<BiomeOffset> built = build_biome_offsets(r, s);
    {
        std::lock_guard<std::mutex> lock(g_biome_offsets_mu);
        const auto [it, _inserted] = g_biome_offsets_cache.emplace(key, std::move(built));
        return it->second;
    }
}

static SCANNER_FORCE_INLINE void push_unique_sculpt_offset(
    std::vector<SculptOffset> &out,
    std::unordered_set<uint64_t> &seen,
    int32_t dx,
    int32_t dz
) {
    const uint64_t key = pack_xz_i32(dx, dz);
    if (seen.insert(key).second) {
        out.push_back(SculptOffset{dx, dz, 0});
    }
}

static std::vector<SculptOffset> build_sculpt_offsets(uint32_t pattern, int32_t radius, int32_t step) {
    const int32_t r = std::max<int32_t>(0, radius);
    const int32_t s = std::max<int32_t>(1, step);
    const int64_t r2 = static_cast<int64_t>(r) * static_cast<int64_t>(r);

    std::vector<SculptOffset> out;
    out.reserve(1024);
    std::unordered_set<uint64_t> seen;
    seen.reserve(1024);

    if (pattern == SCAN_SCULPT_RING) {
        if (r == 0) {
            out.push_back(SculptOffset{0, 0});
            return out;
        }
        constexpr double kPi = 3.14159265358979323846;
        const double circumference = 2.0 * kPi * static_cast<double>(r);
        const int points = std::max<int>(8, static_cast<int>(std::ceil(circumference / static_cast<double>(s))));
        for (int i = 0; i < points; ++i) {
            const double angle = (2.0 * kPi * static_cast<double>(i)) / static_cast<double>(points);
            const int32_t dx = static_cast<int32_t>(std::llround(static_cast<double>(r) * std::cos(angle)));
            const int32_t dz = static_cast<int32_t>(std::llround(static_cast<double>(r) * std::sin(angle)));
            push_unique_sculpt_offset(out, seen, dx, dz);
        }
        if (out.empty()) {
            out.push_back(SculptOffset{0, 0, 0});
        }
        for (SculptOffset &o : out) {
            o.d2 = static_cast<uint64_t>(static_cast<int64_t>(o.dx) * static_cast<int64_t>(o.dx) +
                                         static_cast<int64_t>(o.dz) * static_cast<int64_t>(o.dz));
        }
        return out;
    }

    push_unique_sculpt_offset(out, seen, 0, 0);

    if (pattern == SCAN_SCULPT_GRID) {
        for (int32_t dz = -r; dz <= r; dz += s) {
            for (int32_t dx = -r; dx <= r; dx += s) {
                push_unique_sculpt_offset(out, seen, dx, dz);
            }
        }
        push_unique_sculpt_offset(out, seen, -r, -r);
        push_unique_sculpt_offset(out, seen, r, -r);
        push_unique_sculpt_offset(out, seen, -r, r);
        push_unique_sculpt_offset(out, seen, r, r);
        for (SculptOffset &o : out) {
            o.d2 = static_cast<uint64_t>(static_cast<int64_t>(o.dx) * static_cast<int64_t>(o.dx) +
                                         static_cast<int64_t>(o.dz) * static_cast<int64_t>(o.dz));
        }
        std::sort(out.begin(), out.end(), [](const SculptOffset &a, const SculptOffset &b) { return a.d2 > b.d2; });
        return out;
    }

    if (pattern == SCAN_SCULPT_CROSS) {
        for (int32_t d = -r; d <= r; d += s) {
            push_unique_sculpt_offset(out, seen, d, 0);
            push_unique_sculpt_offset(out, seen, 0, d);
        }
        push_unique_sculpt_offset(out, seen, -r, 0);
        push_unique_sculpt_offset(out, seen, r, 0);
        push_unique_sculpt_offset(out, seen, 0, -r);
        push_unique_sculpt_offset(out, seen, 0, r);
        for (SculptOffset &o : out) {
            o.d2 = static_cast<uint64_t>(static_cast<int64_t>(o.dx) * static_cast<int64_t>(o.dx) +
                                         static_cast<int64_t>(o.dz) * static_cast<int64_t>(o.dz));
        }
        std::sort(out.begin(), out.end(), [](const SculptOffset &a, const SculptOffset &b) { return a.d2 > b.d2; });
        return out;
    }

    // Default: filled disk.
    for (int32_t dz = -r; dz <= r; dz += s) {
        for (int32_t dx = -r; dx <= r; dx += s) {
            const int64_t d2 = static_cast<int64_t>(dx) * static_cast<int64_t>(dx) +
                               static_cast<int64_t>(dz) * static_cast<int64_t>(dz);
            if (d2 > r2) {
                continue;
            }
            push_unique_sculpt_offset(out, seen, dx, dz);
        }
    }
    for (SculptOffset &o : out) {
        o.d2 = static_cast<uint64_t>(static_cast<int64_t>(o.dx) * static_cast<int64_t>(o.dx) +
                                     static_cast<int64_t>(o.dz) * static_cast<int64_t>(o.dz));
    }
    std::sort(out.begin(), out.end(), [](const SculptOffset &a, const SculptOffset &b) { return a.d2 > b.d2; });
    return out;
}

static std::mutex g_sculpt_offsets_mu;
static std::unordered_map<uint64_t, std::vector<SculptOffset>> g_sculpt_offsets_cache;

static SCANNER_FORCE_INLINE uint64_t sculpt_offset_cache_key(uint32_t pattern, int32_t radius, int32_t step) {
    const uint64_t p = static_cast<uint64_t>(pattern & 0xFFU);
    const uint64_t r = static_cast<uint64_t>(static_cast<uint32_t>(std::max<int32_t>(0, radius)));
    const uint64_t s = static_cast<uint64_t>(static_cast<uint32_t>(std::max<int32_t>(1, step)));
    return (p << 56U) | (r << 24U) | s;
}

static const std::vector<SculptOffset> &build_sculpt_offsets_cached(uint32_t pattern, int32_t radius, int32_t step) {
    const uint64_t key = sculpt_offset_cache_key(pattern, radius, step);
    {
        std::lock_guard<std::mutex> lock(g_sculpt_offsets_mu);
        const auto it = g_sculpt_offsets_cache.find(key);
        if (it != g_sculpt_offsets_cache.end()) {
            return it->second;
        }
    }
    std::vector<SculptOffset> built = build_sculpt_offsets(pattern, radius, step);
    {
        std::lock_guard<std::mutex> lock(g_sculpt_offsets_mu);
        const auto [it, _inserted] = g_sculpt_offsets_cache.emplace(key, std::move(built));
        return it->second;
    }
}

static SCANNER_FORCE_INLINE bool insert_unique_small(std::vector<uint64_t> &values, uint64_t value) {
    for (uint64_t existing : values) {
        if (existing == value) {
            return false;
        }
    }
    values.push_back(value);
    return true;
}

static inline std::vector<int32_t> sorted_unique_ids(const int32_t *ids, uint32_t count) {
    std::vector<int32_t> out;
    out.reserve(static_cast<size_t>(count));
    for (uint32_t i = 0; i < count; ++i) {
        out.push_back(ids[i]);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static SCANNER_FORCE_INLINE bool contains_biome_id(const std::vector<int32_t> &sorted_ids, int32_t id) {
    const size_t n = sorted_ids.size();
    if (n == 0U) {
        return false;
    }
    if (n == 1U) {
        return sorted_ids[0] == id;
    }
    if (n == 2U) {
        return sorted_ids[0] == id || sorted_ids[1] == id;
    }
    if (n <= 8U) {
        for (int32_t v : sorted_ids) {
            if (v == id) {
                return true;
            }
        }
        return false;
    }
    return std::binary_search(sorted_ids.begin(), sorted_ids.end(), id);
}

static SCANNER_FORCE_INLINE bool valid_scan_dimension(int32_t dimension) {
    return dimension == DIM_OVERWORLD || dimension == DIM_NETHER || dimension == DIM_END;
}

struct LegacyBiomePrepared {
    bool enabled = false;
    uint32_t point_type = SCAN_ANCHOR_ORIGIN;
    int32_t y = 64;
    int32_t radius = 0;
    std::vector<int32_t> allowed_sorted;
    const std::vector<BiomeOffset> *offsets = nullptr;
};

struct QueryBiomePrepared {
    NativeBiomeFilterDesc desc{};
    std::vector<int32_t> allowed_sorted;
    const std::vector<BiomeOffset> *offsets = nullptr;
};

struct CompiledBiomeGroupFilter {
    uint32_t filter_index = 0U;
    std::vector<uint32_t> sample_indices;
};

struct CompiledBiomeGroup {
    uint32_t point_type = SCAN_ANCHOR_ORIGIN;
    int32_t point_dep_index = -1;
    int32_t dimension = DIM_OVERWORLD;
    int32_t point_x = 0;
    int32_t point_z = 0;
    int32_t y = 64;
    uint32_t sample_step = 1U;
    int32_t envelope_radius = 0;
    std::vector<BiomeOffset> sample_offsets;
    std::vector<CompiledBiomeGroupFilter> filters;
};

struct CompiledQueryPlan {
    std::vector<NativeRegionTerm> regions;
    std::vector<NativeQueryConstraintDesc> constraints;
    std::vector<NativeBiomeFilterDesc> biome_filters;
    std::vector<int32_t> biome_allowed_ids;
    std::vector<QueryBiomePrepared> prepared_filters;
    std::vector<CompiledBiomeGroup> stage_c_biome_groups;
    std::vector<uint32_t> fast_target_caps;
    std::vector<uint32_t> effective_target_caps;
    std::vector<uint8_t> constraint_store_mask;
    std::vector<uint32_t> base_constraint_eval_order;
    std::vector<uint32_t> base_stage0_constraint_eval_order;
    std::vector<uint32_t> stage_a_independent;
    std::vector<uint32_t> stage_a_dependent;
    std::vector<uint32_t> stage_a_driver_order;
    std::vector<uint32_t> stage_a_reject_only_order;
    std::vector<uint8_t> stage_a_exact_anchor_mask;
    std::vector<uint8_t> stage_a_available_mask;
    std::vector<uint32_t> stage_b_exact_structure;
    std::vector<uint32_t> stage_b_stronghold;
    bool has_early_fixed_biome_filters = false;
    bool direct_api_needed = false;
    bool generator_needed = false;
    mutable std::mutex batch_stats_mu;
    mutable ScannerCompiledBatchStats last_batch_stats{};
    mutable std::vector<uint64_t> last_per_constraint_stage_a_rejects;
    mutable std::vector<uint64_t> last_per_constraint_stage_a5_rejects;
    mutable std::vector<uint64_t> last_per_constraint_stage_b_exact_rejects;
};

struct CompiledBatchStatsAccumulator {
    uint64_t input_seed_count = 0ULL;
    uint64_t stage_a_survivor_count = 0ULL;
    uint64_t stage_a5_survivor_count = 0ULL;
    uint64_t stage_b_structure_survivor_count = 0ULL;
    uint64_t stage_c_biome_survivor_count = 0ULL;
    uint64_t final_valid_count = 0ULL;
    uint64_t stage_a_reject_count = 0ULL;
    uint64_t stage_a5_reject_count = 0ULL;
    uint64_t stage_b_exact_reject_count = 0ULL;
    uint64_t stage_c_biome_reject_count = 0ULL;
    double cpu_stage_a5_seconds = 0.0;
    double cpu_stage_b_seconds = 0.0;
    double cpu_stage_c_seconds = 0.0;
    CubiCallCounters cubi_calls{};
    std::vector<uint64_t> per_constraint_stage_a_rejects;
    std::vector<uint64_t> per_constraint_stage_a5_rejects;
    std::vector<uint64_t> per_constraint_stage_b_exact_rejects;

    explicit CompiledBatchStatsAccumulator(uint32_t constraint_count = 0U)
        : per_constraint_stage_a_rejects(constraint_count, 0ULL),
          per_constraint_stage_a5_rejects(constraint_count, 0ULL),
          per_constraint_stage_b_exact_rejects(constraint_count, 0ULL) {}
};

static SCANNER_FORCE_INLINE CompiledQueryPlan *unwrap_query_plan(ScannerCompiledQueryPlan *plan) {
    return plan == nullptr ? nullptr : static_cast<CompiledQueryPlan *>(plan->impl);
}

static SCANNER_FORCE_INLINE const CompiledQueryPlan *unwrap_query_plan(const ScannerCompiledQueryPlan *plan) {
    return plan == nullptr ? nullptr : static_cast<const CompiledQueryPlan *>(plan->impl);
}

static inline bool cubi_get_spawn(
    const CubiApi &api,
    ThreadCubiState &st,
    int mc_version,
    uint64_t seed,
    int32_t &out_x,
    int32_t &out_z
) {
    void *g = ensure_generator(api, st, mc_version, seed, DIM_OVERWORLD);
    if (g == nullptr) {
        return false;
    }
    if (st.call_counters != nullptr) {
        ++st.call_counters->get_spawn_calls;
    }
    const CPos pos = api.get_spawn_direct(g);
    out_x = pos.x;
    out_z = pos.z;
    return true;
}

static inline bool sample_biome_with_offsets(
    const CubiApi &api,
    void *g,
    int32_t y,
    int32_t anchor_x,
    int32_t anchor_z,
    const std::vector<BiomeOffset> &offsets,
    const std::vector<int32_t> &allowed_sorted
) {
    if (g == nullptr) {
        return false;
    }

    for (const BiomeOffset &o : offsets) {
        const int32_t x = anchor_x + o.dx;
        const int32_t z = anchor_z + o.dz;
        const int bid = api.get_biome_at_direct(g, 1, x, y, z);
        if (contains_biome_id(allowed_sorted, bid)) {
            return true;
        }
    }
    return false;
}

static inline bool evaluate_sculpt_seed(
    const CubiApi &api,
    ThreadCubiState &st,
    uint64_t seed,
    int mc_version,
    const NativeSculptDesc &desc,
    const std::vector<SculptOffset> &offsets,
    uint32_t required_hits,
    uint32_t max_misses,
    const std::vector<int32_t> &allowed_sorted
) {
    if (offsets.empty()) {
        return false;
    }
    if (required_hits == 0U) {
        return true;
    }

    void *g = ensure_generator(api, st, mc_version, seed, DIM_OVERWORLD);
    if (g == nullptr) {
        return false;
    }

    int32_t center_x = 0;
    int32_t center_z = 0;
    if (desc.center_type == SCAN_ANCHOR_ORIGIN) {
        center_x = 0;
        center_z = 0;
    } else if (desc.center_type == SCAN_ANCHOR_SPAWN) {
        if (!cubi_get_spawn(api, st, mc_version, seed, center_x, center_z)) {
            return false;
        }
    } else if (desc.center_type == SCAN_ANCHOR_FIXED) {
        center_x = desc.center_x;
        center_z = desc.center_z;
    } else {
        return false;
    }

    uint32_t pass_count = 0U;
    uint32_t miss_count = 0U;
    const bool invert_match = desc.edge_mode == SCAN_EDGE_INWARD;
    for (const SculptOffset &o : offsets) {
        const int32_t sx = center_x + o.dx;
        const int32_t sz = center_z + o.dz;
        const int bid = api.get_biome_at_direct(g, 1, sx, desc.y, sz);
        const bool matched = contains_biome_id(allowed_sorted, bid) != invert_match;
        if (matched) {
            ++pass_count;
            if (pass_count >= required_hits) {
                return true;
            }
        } else {
            ++miss_count;
            if (miss_count > max_misses) {
                return false;
            }
        }
    }
    return pass_count >= required_hits;
}

static inline bool sample_biome_with_offsets_detail(
    const CubiApi &api,
    void *g,
    int32_t y,
    int32_t anchor_x,
    int32_t anchor_z,
    const std::vector<BiomeOffset> &offsets,
    const std::vector<int32_t> &allowed_sorted,
    int32_t &match_x,
    int32_t &match_z,
    int32_t &match_biome_id,
    uint64_t &match_d2
) {
    if (g == nullptr) {
        return false;
    }

    for (const BiomeOffset &o : offsets) {
        const int32_t x = anchor_x + o.dx;
        const int32_t z = anchor_z + o.dz;
        const int bid = api.get_biome_at_direct(g, 1, x, y, z);
        if (contains_biome_id(allowed_sorted, bid)) {
            match_x = x;
            match_z = z;
            match_biome_id = bid;
            match_d2 = o.d2;
            return true;
        }
    }
    return false;
}

static inline bool legacy_seed_has_attempt(
    uint64_t seed,
    const NativeRegionTerm *regions,
    const NativeLegacyContextDesc *contexts,
    uint32_t context_count,
    uint64_t radius_sq
) {
    for (uint32_t c = 0; c < context_count; ++c) {
        const NativeLegacyContextDesc &ctx = contexts[c];
        const uint32_t start = ctx.region_start;
        const uint32_t end = start + ctx.region_count;
        for (uint32_t i = start; i < end; ++i) {
            int32_t x = 0;
            int32_t z = 0;
            structure_attempt_block_pos(seed, regions[i], x, z);
            if (dist2_i64(x, z, 0, 0) <= radius_sq) {
                return true;
            }
        }
    }
    return false;
}

static inline bool legacy_seed_has_viable(
    const CubiApi &api,
    ThreadCubiState &st,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    const NativeLegacyContextDesc *contexts,
    uint32_t context_count,
    uint64_t radius_sq
) {
    void *g = ensure_generator(api, st, mc_version, seed, DIM_OVERWORLD);
    if (g == nullptr) {
        return false;
    }

    for (uint32_t c = 0; c < context_count; ++c) {
        const NativeLegacyContextDesc &ctx = contexts[c];
        if (ctx.struct_id < 0) {
            continue;
        }
        const uint32_t start = ctx.region_start;
        const uint32_t end = start + ctx.region_count;
        for (uint32_t i = start; i < end; ++i) {
            const NativeRegionTerm &region = regions[i];
            CPos pos{};
            if (!api.get_structure_pos_direct(
                    ctx.struct_id,
                    mc_version,
                    seed,
                    region.rx,
                    region.rz,
                    &pos
                )) {
                continue;
            }
            const int32_t x = pos.x;
            const int32_t z = pos.z;

            if (dist2_i64(x, z, 0, 0) > radius_sq) {
                continue;
            }
            if (!structure_has_viability_check(ctx.struct_id) ||
                api.is_viable_structure_pos_direct(ctx.struct_id, g, x, z, 0U) == 1) {
                return true;
            }
        }
    }
    return false;
}

static inline bool legacy_closest_attempt(
    uint64_t seed,
    const NativeRegionTerm *regions,
    const NativeLegacyContextDesc *contexts,
    uint32_t context_count,
    uint64_t radius_sq,
    int32_t &best_x,
    int32_t &best_z
) {
    bool has_best = false;
    uint64_t best_d2 = 0;

    for (uint32_t c = 0; c < context_count; ++c) {
        const NativeLegacyContextDesc &ctx = contexts[c];
        const uint32_t start = ctx.region_start;
        const uint32_t end = start + ctx.region_count;
        for (uint32_t i = start; i < end; ++i) {
            int32_t x = 0;
            int32_t z = 0;
            structure_attempt_block_pos(seed, regions[i], x, z);
            const uint64_t d2 = dist2_i64(x, z, 0, 0);
            if (d2 > radius_sq) {
                continue;
            }
            if (!has_best || d2 < best_d2) {
                has_best = true;
                best_d2 = d2;
                best_x = x;
                best_z = z;
            }
        }
    }
    return has_best;
}

static inline bool legacy_closest_viable(
    const CubiApi &api,
    ThreadCubiState &st,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    const NativeLegacyContextDesc *contexts,
    uint32_t context_count,
    uint64_t radius_sq,
    int32_t &best_x,
    int32_t &best_z
) {
    void *g = ensure_generator(api, st, mc_version, seed, DIM_OVERWORLD);
    if (g == nullptr) {
        return false;
    }

    bool has_best = false;
    uint64_t best_d2 = 0;
    for (uint32_t c = 0; c < context_count; ++c) {
        const NativeLegacyContextDesc &ctx = contexts[c];
        if (ctx.struct_id < 0) {
            continue;
        }
        const uint32_t start = ctx.region_start;
        const uint32_t end = start + ctx.region_count;
        for (uint32_t i = start; i < end; ++i) {
            const NativeRegionTerm &region = regions[i];
            CPos pos{};
            if (!api.get_structure_pos_direct(
                    ctx.struct_id,
                    mc_version,
                    seed,
                    region.rx,
                    region.rz,
                    &pos
                )) {
                continue;
            }
            const int32_t x = pos.x;
            const int32_t z = pos.z;

            const uint64_t d2 = dist2_i64(x, z, 0, 0);
            if (d2 > radius_sq) {
                continue;
            }
            if (structure_has_viability_check(ctx.struct_id) &&
                api.is_viable_structure_pos_direct(ctx.struct_id, g, x, z, 0U) != 1) {
                continue;
            }
            if (!has_best || d2 < best_d2) {
                has_best = true;
                best_d2 = d2;
                best_x = x;
                best_z = z;
            }
        }
    }
    return has_best;
}

enum class SeedStatus : uint8_t {
    valid = 0,
    reject = 1,
    biome_reject = 2,
};

static SeedStatus evaluate_legacy_seed(
    const CubiApi &api,
    ThreadCubiState &st,
    uint64_t seed,
    const NativeRegionTerm *regions,
    const NativeLegacyContextDesc *contexts,
    uint32_t context_count,
    uint64_t radius_sq,
    bool strict_viability,
    int mc_version,
    const LegacyBiomePrepared &biome
) {
    bool ok = false;
    if (strict_viability) {
        ok = legacy_seed_has_viable(api, st, seed, mc_version, regions, contexts, context_count, radius_sq);
    } else {
        ok = legacy_seed_has_attempt(seed, regions, contexts, context_count, radius_sq);
    }
    if (!ok) {
        return SeedStatus::reject;
    }
    if (!biome.enabled) {
        return SeedStatus::valid;
    }
    void *g_biome = ensure_generator(api, st, mc_version, seed, DIM_OVERWORLD);
    if (g_biome == nullptr) {
        return SeedStatus::biome_reject;
    }

    int32_t anchor_x = 0;
    int32_t anchor_z = 0;
    if (biome.point_type == SCAN_ANCHOR_ORIGIN) {
        anchor_x = 0;
        anchor_z = 0;
    } else if (biome.point_type == SCAN_ANCHOR_SPAWN) {
        if (!cubi_get_spawn(api, st, mc_version, seed, anchor_x, anchor_z)) {
            return SeedStatus::biome_reject;
        }
    } else {
        bool has_closest = false;
        if (strict_viability) {
            has_closest = legacy_closest_viable(
                api,
                st,
                seed,
                mc_version,
                regions,
                contexts,
                context_count,
                radius_sq,
                anchor_x,
                anchor_z
            );
        } else {
            has_closest =
                legacy_closest_attempt(seed, regions, contexts, context_count, radius_sq, anchor_x, anchor_z);
        }
        if (!has_closest) {
            return SeedStatus::biome_reject;
        }
    }
    if (biome.offsets == nullptr) {
        return SeedStatus::biome_reject;
    }

    if (!sample_biome_with_offsets(
            api,
            g_biome,
            biome.y,
            anchor_x,
            anchor_z,
            *biome.offsets,
            biome.allowed_sorted
        )) {
        return SeedStatus::biome_reject;
    }
    return SeedStatus::valid;
}

struct QueryScratch {
    std::vector<std::vector<uint64_t>> matches_by_constraint;
    std::vector<int32_t> grouped_biome_ids;
    CubiCallCounters *call_counters = nullptr;
    struct CandidateDedupe {
        std::vector<uint64_t> linear_values;
        std::unordered_set<uint64_t> large;
        bool use_hash = false;

        void prepare(uint32_t expected) {
            constexpr uint32_t kHashThreshold = 96U;
            use_hash = expected > kHashThreshold;
            if (use_hash) {
                large.clear();
                large.reserve(static_cast<size_t>(expected));
            } else {
                linear_values.clear();
                if (linear_values.capacity() < static_cast<size_t>(expected)) {
                    linear_values.reserve(static_cast<size_t>(expected));
                }
            }
        }

        void clear() {
            linear_values.clear();
            large.clear();
            use_hash = false;
        }

        SCANNER_FORCE_INLINE bool insert(uint64_t value) {
            if (use_hash) {
                return large.insert(value).second;
            }
            for (uint64_t existing : linear_values) {
                if (existing == value) {
                    return false;
                }
            }
            linear_values.push_back(value);
            return true;
        }
    };
    CandidateDedupe dedupe;
    CandidateDedupe constraint_seen_dedupe;
    struct BiomeDirectCacheEntry {
        uint64_t key = 0ULL;
        uint32_t epoch = 0U;
        int32_t biome_id = 0;
    };
    struct StructureDirectCacheEntry {
        uint64_t key = 0ULL;
        uint32_t epoch = 0U;
        int32_t x = 0;
        int32_t z = 0;
        uint8_t pos_ready = 0U;
        uint8_t pos_ok = 0U;
        uint8_t viable_ready = 0U;
        uint8_t viable = 0U;
    };
    static constexpr size_t kBiomeDirectCacheSize = 1024U;
    static constexpr size_t kStructureDirectCacheSize = 1024U;
    std::array<BiomeDirectCacheEntry, kBiomeDirectCacheSize> biome_cache{};
    std::array<StructureDirectCacheEntry, kStructureDirectCacheSize> structure_cache{};
    uint32_t cache_epoch = 1U;
    struct StrongholdCache {
        bool ready = false;
        uint64_t seed = 0;
        int mc_version = -1;
        StrongholdIterNative iter{};
        std::array<CPos, 128> pos{};
        std::array<uint8_t, 128> ring{};
        uint32_t generated = 0;

        void reset() {
            ready = false;
            seed = 0;
            mc_version = -1;
            generated = 0;
        }
    };
    StrongholdCache strongholds;
    bool spawn_cached = false;
    int32_t spawn_x = 0;
    int32_t spawn_z = 0;
    bool spawn_estimate_cached = false;
    int32_t spawn_estimate_x = 0;
    int32_t spawn_estimate_z = 0;

    QueryScratch(uint32_t constraint_count, const NativeQueryConstraintDesc *constraints)
        : matches_by_constraint(constraint_count) {
        uint32_t max_cap = 0U;
        uint32_t max_any_cap = 0U;
        if (constraints != nullptr) {
            for (uint32_t i = 0; i < constraint_count; ++i) {
                const uint32_t cap = constraints[i].candidate_cap;
                matches_by_constraint[i].reserve(static_cast<size_t>(cap));
                if (cap > max_any_cap) {
                    max_any_cap = cap;
                }
                if (cap > max_cap && cap <= 96U) {
                    max_cap = cap;
                }
            }
        }
        if (max_cap > 0U) {
            dedupe.linear_values.reserve(static_cast<size_t>(max_cap));
            constraint_seen_dedupe.linear_values.reserve(static_cast<size_t>(max_cap));
        }
        if (max_any_cap > 96U) {
            dedupe.large.reserve(static_cast<size_t>(max_any_cap));
            constraint_seen_dedupe.large.reserve(static_cast<size_t>(max_any_cap));
        }
    }

    void reset() {
        strongholds.reset();
        spawn_cached = false;
        spawn_x = 0;
        spawn_z = 0;
        spawn_estimate_cached = false;
        spawn_estimate_x = 0;
        spawn_estimate_z = 0;
        ++cache_epoch;
        if (cache_epoch == 0U) {
            cache_epoch = 1U;
            for (BiomeDirectCacheEntry &entry : biome_cache) {
                entry.epoch = 0U;
            }
            for (StructureDirectCacheEntry &entry : structure_cache) {
                entry.epoch = 0U;
            }
        }
    }
};

static SCANNER_FORCE_INLINE uint64_t mix_cache_key64(uint64_t value) {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

static SCANNER_FORCE_INLINE uint64_t biome_direct_cache_key(int32_t dimension, int32_t x, int32_t y, int32_t z) {
    uint64_t key = pack_xz_i32(x, z);
    key ^= static_cast<uint64_t>(static_cast<uint32_t>(y)) * 0x9e3779b185ebca87ULL;
    key ^= static_cast<uint64_t>(static_cast<uint32_t>(dimension)) * 0x94d049bb133111ebULL;
    return mix_cache_key64(key);
}

static SCANNER_FORCE_INLINE uint64_t structure_direct_cache_key(
    int32_t dimension,
    int32_t struct_id,
    int32_t rx,
    int32_t rz,
    uint32_t flags = 0U
) {
    uint64_t key = pack_xz_i32(rx, rz);
    key ^= static_cast<uint64_t>(static_cast<uint32_t>(struct_id)) * 0xd6e8feb86659fd93ULL;
    key ^= static_cast<uint64_t>(static_cast<uint32_t>(dimension)) * 0x9e3779b185ebca87ULL;
    key ^= static_cast<uint64_t>(flags) * 0x94d049bb133111ebULL;
    return mix_cache_key64(key);
}

static SCANNER_FORCE_INLINE int cached_query_biome_at(
    const CubiApi &api,
    QueryScratch &scratch,
    void *g,
    int32_t dimension,
    int32_t x,
    int32_t y,
    int32_t z
) {
    const uint64_t key = biome_direct_cache_key(dimension, x, y, z);
    QueryScratch::BiomeDirectCacheEntry &entry =
        scratch.biome_cache[static_cast<size_t>(key) & (QueryScratch::kBiomeDirectCacheSize - 1U)];
    if (entry.epoch == scratch.cache_epoch && entry.key == key) {
        return entry.biome_id;
    }
    if (scratch.call_counters != nullptr) {
        ++scratch.call_counters->get_biome_at_calls;
    }
    const int biome_id = api.get_biome_at_direct(g, 1, x, y, z);
    entry.key = key;
    entry.epoch = scratch.cache_epoch;
    entry.biome_id = biome_id;
    return biome_id;
}

static SCANNER_FORCE_INLINE bool cached_get_structure_pos_direct(
    const CubiApi &api,
    QueryScratch &scratch,
    int32_t struct_id,
    int32_t dimension,
    int mc_version,
    uint64_t seed,
    int32_t rx,
    int32_t rz,
    CPos &out
) {
    const uint64_t key = structure_direct_cache_key(dimension, struct_id, rx, rz);
    QueryScratch::StructureDirectCacheEntry &entry =
        scratch.structure_cache[static_cast<size_t>(key) & (QueryScratch::kStructureDirectCacheSize - 1U)];
    if (entry.epoch == scratch.cache_epoch && entry.key == key && entry.pos_ready != 0U) {
        if (entry.pos_ok != 0U) {
            out.x = entry.x;
            out.z = entry.z;
            return true;
        }
        return false;
    }
    if (scratch.call_counters != nullptr) {
        ++scratch.call_counters->get_structure_pos_calls;
    }
    const bool ok = api.get_structure_pos_direct(struct_id, mc_version, seed, rx, rz, &out) != 0;
    entry.key = key;
    entry.epoch = scratch.cache_epoch;
    entry.pos_ready = 1U;
    entry.pos_ok = ok ? 1U : 0U;
    entry.viable_ready = 0U;
    if (ok) {
        entry.x = out.x;
        entry.z = out.z;
    }
    return ok;
}

static SCANNER_FORCE_INLINE bool cached_is_viable_structure_pos_direct(
    const CubiApi &api,
    QueryScratch &scratch,
    int32_t struct_id,
    int32_t dimension,
    int mc_version,
    uint64_t seed,
    void *g_world,
    int32_t rx,
    int32_t rz,
    int32_t x,
    int32_t z,
    uint32_t flags
) {
    (void)mc_version;
    (void)seed;
    const uint64_t key = structure_direct_cache_key(dimension, struct_id, rx, rz, flags);
    QueryScratch::StructureDirectCacheEntry &entry =
        scratch.structure_cache[static_cast<size_t>(key) & (QueryScratch::kStructureDirectCacheSize - 1U)];
    if (entry.epoch == scratch.cache_epoch && entry.key == key && entry.viable_ready != 0U) {
        return entry.viable != 0U;
    }
    if (scratch.call_counters != nullptr) {
        ++scratch.call_counters->is_viable_structure_pos_calls;
    }
    const bool viable = api.is_viable_structure_pos_direct(struct_id, g_world, x, z, flags) == 1;
    entry.key = key;
    entry.epoch = scratch.cache_epoch;
    entry.x = x;
    entry.z = z;
    entry.pos_ready = 1U;
    entry.pos_ok = 1U;
    entry.viable_ready = 1U;
    entry.viable = viable ? 1U : 0U;
    return viable;
}

static SCANNER_FORCE_INLINE bool init_stronghold_cache_if_needed(
    const CubiApi &api,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    void *g_world
) {
    auto &cache = scratch.strongholds;
    if (cache.ready && cache.seed == seed && cache.mc_version == mc_version) {
        return true;
    }
    cache.ready = false;
    cache.seed = seed;
    cache.mc_version = mc_version;
    cache.generated = 0;
    if (g_world == nullptr) {
        return false;
    }

    cache.iter = StrongholdIterNative{};
    if (scratch.call_counters != nullptr) {
        ++scratch.call_counters->init_first_stronghold_calls;
    }
    api.init_first_stronghold(&cache.iter, mc_version, seed);
    cache.ready = true;
    return true;
}

static SCANNER_FORCE_INLINE bool stronghold_ring_range_mc1_9_plus(
    int32_t anchor_x,
    int32_t anchor_z,
    uint64_t radius_sq,
    int &out_min_ring,
    int &out_max_ring
) {
    // Vanilla stronghold ring radii ranges (MC 1.9+), in blocks:
    // r = 1408 + 3072*n + 1280*[0,1] (+/-112). Add a small safety margin.
    constexpr long double kBase = 1408.0L;
    constexpr long double kRingStep = 3072.0L;
    constexpr long double kRingSpan = 1280.0L;
    constexpr long double kMargin = 128.0L; // 112 biome adjust + 8 chunk-center + slack.

    const long double ax = static_cast<long double>(anchor_x);
    const long double az = static_cast<long double>(anchor_z);
    const long double d = std::sqrt(ax * ax + az * az);
    const long double r = std::sqrt(static_cast<long double>(radius_sq));

    long double imin = d - r - kMargin;
    if (imin < 0.0L) {
        imin = 0.0L;
    }
    const long double imax = d + r + kMargin;
    if (imax < kBase) {
        return false;
    }

    const long double min_ring_f = (imin - (kBase + kRingSpan)) / kRingStep;
    const long double max_ring_f = (imax - kBase) / kRingStep;
    int min_ring = static_cast<int>(std::ceil(min_ring_f));
    int max_ring = static_cast<int>(std::floor(max_ring_f));
    if (min_ring < 0) {
        min_ring = 0;
    }
    if (max_ring < 0) {
        return false;
    }
    if (min_ring > max_ring) {
        return false;
    }
    out_min_ring = min_ring;
    out_max_ring = max_ring;
    return true;
}

struct DetailPoint {
    int32_t x;
    int32_t z;
    int32_t anchor_x;
    int32_t anchor_z;
};

struct DetailKey {
    int32_t x;
    int32_t z;
    int32_t anchor_x;
    int32_t anchor_z;
};

static inline bool operator==(const DetailKey &a, const DetailKey &b) noexcept {
    return a.x == b.x && a.z == b.z && a.anchor_x == b.anchor_x && a.anchor_z == b.anchor_z;
}

static SCANNER_FORCE_INLINE bool insert_unique_small_detail(std::vector<DetailKey> &values, const DetailKey &value) {
    for (const DetailKey &existing : values) {
        if (existing == value) {
            return false;
        }
    }
    values.push_back(value);
    return true;
}

static SCANNER_FORCE_INLINE bool cubi_get_spawn_from_generator(
    const CubiApi &api,
    void *g,
    int32_t &out_x,
    int32_t &out_z
) {
    if (g == nullptr) {
        return false;
    }
    const CPos pos = api.get_spawn_direct(g);
    out_x = pos.x;
    out_z = pos.z;
    return true;
}

static SCANNER_FORCE_INLINE bool cubi_estimate_spawn_from_generator(
    const CubiApi &api,
    void *g,
    int32_t &out_x,
    int32_t &out_z
) {
    if (g == nullptr || api.estimate_spawn_direct == nullptr) {
        return false;
    }
    uint64_t rng = 0ULL;
    const CPos pos = api.estimate_spawn_direct(g, &rng);
    out_x = pos.x;
    out_z = pos.z;
    return true;
}

static SCANNER_FORCE_INLINE bool query_get_spawn(
    const CubiApi &api,
    ThreadCubiState &st,
    int mc_version,
    uint64_t seed,
    QueryScratch &scratch,
    void *&g_world,
    int32_t &out_x,
    int32_t &out_z
) {
    if (scratch.spawn_cached) {
        out_x = scratch.spawn_x;
        out_z = scratch.spawn_z;
        return true;
    }
    g_world = ensure_generator(api, st, mc_version, seed, DIM_OVERWORLD);
    if (st.call_counters != nullptr) {
        ++st.call_counters->get_spawn_calls;
    }
    if (!cubi_get_spawn_from_generator(api, g_world, scratch.spawn_x, scratch.spawn_z)) {
        return false;
    }
    scratch.spawn_cached = true;
    out_x = scratch.spawn_x;
    out_z = scratch.spawn_z;
    return true;
}

static SCANNER_FORCE_INLINE bool query_get_spawn_estimate(
    const CubiApi &api,
    ThreadCubiState &st,
    int mc_version,
    uint64_t seed,
    QueryScratch &scratch,
    void *&g_world,
    int32_t &out_x,
    int32_t &out_z
) {
    if (scratch.spawn_estimate_cached) {
        out_x = scratch.spawn_estimate_x;
        out_z = scratch.spawn_estimate_z;
        return true;
    }
    g_world = ensure_generator(api, st, mc_version, seed, DIM_OVERWORLD);
    if (st.call_counters != nullptr) {
        ++st.call_counters->estimate_spawn_calls;
    }
    if (!cubi_estimate_spawn_from_generator(api, g_world, scratch.spawn_estimate_x, scratch.spawn_estimate_z)) {
        return false;
    }
    scratch.spawn_estimate_cached = true;
    out_x = scratch.spawn_estimate_x;
    out_z = scratch.spawn_estimate_z;
    return true;
}

static SCANNER_FORCE_INLINE bool region_aabb_outside_radius(
    int32_t rx,
    int32_t rz,
    int32_t spacing_chunks,
    int32_t anchor_x,
    int32_t anchor_z,
    uint64_t radius_sq
) {
    const int64_t min_x = static_cast<int64_t>(rx) * spacing_chunks * CHUNK_SIZE;
    const int64_t max_x = static_cast<int64_t>(rx + 1) * spacing_chunks * CHUNK_SIZE - 1;
    const int64_t min_z = static_cast<int64_t>(rz) * spacing_chunks * CHUNK_SIZE;
    const int64_t max_z = static_cast<int64_t>(rz + 1) * spacing_chunks * CHUNK_SIZE - 1;

    int64_t dx = 0;
    if (anchor_x < min_x) dx = min_x - anchor_x;
    else if (anchor_x > max_x) dx = anchor_x - max_x;

    int64_t dz = 0;
    if (anchor_z < min_z) dz = min_z - anchor_z;
    else if (anchor_z > max_z) dz = anchor_z - max_z;

    const uint64_t dist2 = static_cast<uint64_t>(dx * dx + dz * dz);
    return dist2 > radius_sq;
}

static SCANNER_FORCE_INLINE bool biome_filter_uses_exact_early_anchor(const NativeBiomeFilterDesc &desc) {
    // Only lift explicit point checks whose anchor is known before structure evaluation.
    // Spawn filters use a separate reject-only area prefilter so we can avoid getSpawn()
    // until the precise path is actually needed.
    return desc.point_type == SCAN_ANCHOR_ORIGIN || desc.point_type == SCAN_ANCHOR_FIXED;
}

static SCANNER_FORCE_INLINE bool biome_filter_is_cheap_exact_early_anchor(const QueryBiomePrepared &bf) {
    return biome_filter_uses_exact_early_anchor(bf.desc) &&
           bf.offsets != nullptr &&
           bf.offsets->size() <= EARLY_FIXED_BIOME_MAX_SAMPLES;
}

static SCANNER_FORCE_INLINE bool constraint_region_outside_radius(
    const NativeQueryConstraintDesc &rc,
    const NativeRegionTerm &region,
    int32_t anchor_x,
    int32_t anchor_z
) {
    if (rc.region_spacing_chunks == 0U) {
        return false;
    }
    return region_aabb_outside_radius(
        region.rx,
        region.rz,
        std::max<int32_t>(1, static_cast<int32_t>(rc.region_spacing_chunks)),
        anchor_x,
        anchor_z,
        rc.candidate_radius_sq
    );
}

static SCANNER_FORCE_INLINE bool query_biome_matches_with_offsets_at_point(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    void *&g_world,
    int32_t dimension,
    int32_t y,
    const std::vector<BiomeOffset> &offsets,
    const std::vector<int32_t> &allowed_sorted,
    int32_t point_x,
    int32_t point_z
) {
    g_world = ensure_generator(api, st, mc_version, seed, dimension);
    if (g_world == nullptr) {
        return false;
    }
    for (const BiomeOffset &o : offsets) {
        const int32_t x = point_x + o.dx;
        const int32_t z = point_z + o.dz;
        const int bid = cached_query_biome_at(api, scratch, g_world, dimension, x, y, z);
        if (contains_biome_id(allowed_sorted, bid)) {
            return true;
        }
    }
    return false;
}

static SCANNER_FORCE_INLINE bool query_biome_filter_matches_at_point(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    void *&g_world,
    const QueryBiomePrepared &bf,
    int32_t point_x,
    int32_t point_z
) {
    if (bf.offsets == nullptr) {
        return false;
    }
    return query_biome_matches_with_offsets_at_point(
        api,
        st,
        scratch,
        seed,
        mc_version,
        g_world,
        bf.desc.dimension,
        bf.desc.y,
        *bf.offsets,
        bf.allowed_sorted,
        point_x,
        point_z
    );
}

static SCANNER_FORCE_INLINE bool resolve_exact_early_biome_anchor_point(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    void *&g_world,
    const NativeBiomeFilterDesc &desc,
    int32_t &out_x,
    int32_t &out_z
) {
    if (desc.point_type == SCAN_ANCHOR_ORIGIN) {
        out_x = 0;
        out_z = 0;
        return true;
    }
    if (desc.point_type == SCAN_ANCHOR_FIXED) {
        out_x = desc.point_x;
        out_z = desc.point_z;
        return true;
    }
    return false;
}

template <typename EmitFn>
static SCANNER_FORCE_INLINE uint32_t collect_constraint_candidates_stronghold_emit(
    const CubiApi &api,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc &rc,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t target_cap,
    bool count_cap_prune,
    void *g_world,
    EmitFn &&emit
) {
    (void)regions;
    (void)region_count;
    QueryScratch::CandidateDedupe &dedupe = scratch.dedupe;
    uint32_t cap_pruned = 0;
    if (!init_stronghold_cache_if_needed(api, scratch, seed, mc_version, g_world)) {
        return cap_pruned;
    }
    QueryScratch::StrongholdCache &sh_cache = scratch.strongholds;
    int min_ring = 0;
    int max_ring = 0;
    if (!stronghold_ring_range_mc1_9_plus(anchor_x, anchor_z, rc.candidate_radius_sq, min_ring, max_ring)) {
        return cap_pruned;
    }
    const int32_t chunk_limit = static_cast<int32_t>(rc.candidate_chunk_radius) + 2;
    uint32_t accepted = 0;

    for (uint32_t i = 0; i < sh_cache.generated; ++i) {
        const int ring = static_cast<int>(sh_cache.ring[i]);
        if (ring < min_ring) {
            continue;
        }
        if (ring > max_ring) {
            break;
        }
        const CPos &p = sh_cache.pos[i];
        const int32_t x = p.x;
        const int32_t z = p.z;
        const int32_t cdx = std::abs(x - anchor_x) >> 4;
        const int32_t cdz = std::abs(z - anchor_z) >> 4;
        if (cdx > chunk_limit || cdz > chunk_limit) {
            continue;
        }
        if (dist2_i64(x, z, anchor_x, anchor_z) > rc.candidate_radius_sq) {
            continue;
        }
        const uint64_t packed = pack_xz_i32(x, z);
        if (!dedupe.insert(packed)) {
            continue;
        }
        emit(packed);
        ++accepted;
        if (accepted >= target_cap) {
            if (count_cap_prune) {
                ++cap_pruned;
            }
            break;
        }
    }

    while (accepted < target_cap && sh_cache.generated < static_cast<uint32_t>(sh_cache.pos.size())) {
        if (scratch.call_counters != nullptr) {
            ++scratch.call_counters->next_stronghold_calls;
        }
        const int ring = std::max<int>(0, sh_cache.iter.ringnum);
        api.next_stronghold(&sh_cache.iter, g_world);
        const CPos p = sh_cache.iter.pos;
        sh_cache.pos[sh_cache.generated] = p;
        sh_cache.ring[sh_cache.generated] = static_cast<uint8_t>(std::min<int>(255, ring));
        ++sh_cache.generated;

        if (ring > max_ring) {
            break;
        }
        if (ring < min_ring) {
            continue;
        }

        const int32_t x = p.x;
        const int32_t z = p.z;
        const int32_t cdx = std::abs(x - anchor_x) >> 4;
        const int32_t cdz = std::abs(z - anchor_z) >> 4;
        if (cdx > chunk_limit || cdz > chunk_limit) {
            continue;
        }
        if (dist2_i64(x, z, anchor_x, anchor_z) > rc.candidate_radius_sq) {
            continue;
        }
        const uint64_t packed = pack_xz_i32(x, z);
        if (!dedupe.insert(packed)) {
            continue;
        }
        emit(packed);
        ++accepted;
        if (accepted >= target_cap) {
            if (count_cap_prune) {
                ++cap_pruned;
            }
            break;
        }
    }
    return cap_pruned;
}

template <typename EmitFn>
static SCANNER_FORCE_INLINE uint32_t collect_constraint_candidates_direct_no_prefilter_emit(
    const CubiApi &api,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc &rc,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t target_cap,
    bool count_cap_prune,
    bool require_viability,
    void *g_world,
    EmitFn &&emit
) {
    (void)regions;
    (void)region_count;
    QueryScratch::CandidateDedupe &dedupe = scratch.dedupe;
    uint32_t cap_pruned = 0;
    if (rc.struct_id < 0) {
        return cap_pruned;
    }
    if (require_viability && g_world == nullptr) {
        return cap_pruned;
    }

    const int32_t spacing = std::max<int32_t>(1, static_cast<int32_t>(rc.strict_region_spacing_hint));
    const int32_t anchor_cx = floor_div_i32(anchor_x, CHUNK_SIZE);
    const int32_t anchor_cz = floor_div_i32(anchor_z, CHUNK_SIZE);
    const int32_t anchor_rx = floor_div_i32(anchor_cx, spacing);
    const int32_t anchor_rz = floor_div_i32(anchor_cz, spacing);
    const int32_t region_radius = std::max<int32_t>(
        2,
        static_cast<int32_t>((rc.candidate_chunk_radius + static_cast<uint32_t>(spacing) - 1U) /
                             static_cast<uint32_t>(spacing)) +
            2
    );

    const int32_t srx = anchor_rx - region_radius;
    const int32_t erx = anchor_rx + region_radius + 1;
    const int32_t srz = anchor_rz - region_radius;
    const int32_t erz = anchor_rz + region_radius + 1;

    uint32_t accepted = 0;
    auto try_region = [&](int32_t rx, int32_t rz) {
        // Safe geometric prune before Cubi call
        if (region_aabb_outside_radius(
                rx,
                rz,
                spacing,
                anchor_x,
                anchor_z,
                rc.candidate_radius_sq)) {
            if (require_viability) {
                debug_validate_region_prune_rejection(
                    "strict_no_prefilter",
                    api,
                    seed,
                    mc_version,
                    rc,
                    rx,
                    rz,
                    anchor_x,
                    anchor_z,
                    g_world
                );
            }
            return false;
        }
        if (!direct_structure_attempt_within_radius(seed, rc, rx, rz, anchor_x, anchor_z)) {
            if (require_viability) {
                debug_validate_attempt_prefilter_rejection(
                    "strict_no_prefilter",
                    api,
                    seed,
                    mc_version,
                    rc,
                    rx,
                    rz,
                    anchor_x,
                    anchor_z,
                    g_world
                );
            }
            return false;
        }

        CPos pos{};
        if (!cached_get_structure_pos_direct(api, scratch, rc.struct_id, rc.dimension, mc_version, seed, rx, rz, pos)) {
            return false;
        }
        const int32_t x = pos.x;
        const int32_t z = pos.z;
        if (require_viability && structure_has_viability_check(rc.struct_id) &&
            !cached_is_viable_structure_pos_direct(
                api,
                scratch,
                rc.struct_id,
                rc.dimension,
                mc_version,
                seed,
                g_world,
                rx,
                rz,
                x,
                z,
                rc.viability_flags)) {
            return false;
        }
        if (dist2_i64(x, z, anchor_x, anchor_z) > rc.candidate_radius_sq) {
            return false;
        }
        const uint64_t packed = pack_xz_i32(x, z);
        if (!dedupe.insert(packed)) {
            return false;
        }
        emit(packed);
        ++accepted;
        return accepted >= target_cap;
    };

    if (target_cap == 1U) {
        for (int32_t r = 0; r <= region_radius; ++r) {
            const int32_t minx = anchor_rx - r;
            const int32_t maxx = anchor_rx + r;
            const int32_t minz = anchor_rz - r;
            const int32_t maxz = anchor_rz + r;

            for (int32_t rx = minx; rx <= maxx; ++rx) {
                if (try_region(rx, minz)) {
                    if (count_cap_prune) {
                        ++cap_pruned;
                    }
                    return cap_pruned;
                }
                if (maxz != minz && try_region(rx, maxz)) {
                    if (count_cap_prune) {
                        ++cap_pruned;
                    }
                    return cap_pruned;
                }
            }
            for (int32_t rz = minz + 1; rz <= maxz - 1; ++rz) {
                if (try_region(minx, rz)) {
                    if (count_cap_prune) {
                        ++cap_pruned;
                    }
                    return cap_pruned;
                }
                if (maxx != minx && try_region(maxx, rz)) {
                    if (count_cap_prune) {
                        ++cap_pruned;
                    }
                    return cap_pruned;
                }
            }
        }
        return cap_pruned;
    }

    for (int32_t rx = srx; rx < erx; ++rx) {
        for (int32_t rz = srz; rz < erz; ++rz) {
            if (!try_region(rx, rz)) {
                continue;
            }
            if (count_cap_prune) {
                ++cap_pruned;
            }
            return cap_pruned;
        }
    }
    return cap_pruned;
}

static SCANNER_FORCE_INLINE bool constraint_needs_generator(const NativeQueryConstraintDesc &rc) {
    return constraint_requires_exact_generator(rc);
}

static SCANNER_FORCE_INLINE bool constraint_needs_direct_api(const NativeQueryConstraintDesc &rc) {
    return constraint_requires_exact_generator(rc);
}

static SCANNER_FORCE_INLINE bool constraint_supports_stage0_attempt_prefilter(const NativeQueryConstraintDesc &rc) {
    if (rc.is_stronghold != 0U) {
        return false;
    }
    if (rc.anchor_type != SCAN_ANCHOR_ORIGIN && rc.anchor_type != SCAN_ANCHOR_FIXED) {
        return false;
    }
    if (rc.prefilter_supported != 0U) {
        return rc.mode_strict == 0U || rc.exact_attempt_prefilter != 0U;
    }
    return rc.exact_attempt_prefilter != 0U;
}

static std::vector<uint32_t> build_stage0_constraint_eval_order(
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const uint32_t *constraint_eval_order
) {
    std::vector<uint32_t> out;
    out.reserve(constraint_count);
    if (constraint_count == 0U || constraints == nullptr) {
        return out;
    }
    for (uint32_t ord = 0; ord < constraint_count; ++ord) {
        const uint32_t idx = constraint_eval_order == nullptr ? ord : constraint_eval_order[ord];
        if (idx >= constraint_count) {
            break;
        }
        if (constraint_supports_stage0_attempt_prefilter(constraints[idx])) {
            out.push_back(idx);
        }
    }
    return out;
}

static SCANNER_FORCE_INLINE uint32_t count_constraint_stage0_region_list_candidates(
    uint64_t seed,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc &rc,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t stop_after
) {
    if (rc.region_count == 0U || regions == nullptr || stop_after == 0U) {
        return 0U;
    }

    const uint32_t start = rc.region_start;
    const uint32_t end = std::min<uint32_t>(region_count, start + rc.region_count);
    uint32_t accepted = 0U;
    for (uint32_t i = start; i < end; ++i) {
        const NativeRegionTerm &region = regions[i];
        if (constraint_region_outside_radius(rc, region, anchor_x, anchor_z)) {
            continue;
        }
        if (!structure_attempt_within_radius(seed, region, anchor_x, anchor_z, rc.candidate_radius_sq)) {
            continue;
        }
        ++accepted;
        if (accepted >= stop_after) {
            break;
        }
    }
    return accepted;
}

static SCANNER_FORCE_INLINE uint32_t count_constraint_stage0_direct_candidates(
    uint64_t seed,
    const NativeQueryConstraintDesc &rc,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t stop_after
) {
    if (rc.struct_id < 0 || rc.exact_attempt_prefilter == 0U || stop_after == 0U) {
        return 0U;
    }

    const int32_t spacing = std::max<int32_t>(1, static_cast<int32_t>(rc.strict_region_spacing_hint));
    const int32_t anchor_cx = floor_div_i32(anchor_x, CHUNK_SIZE);
    const int32_t anchor_cz = floor_div_i32(anchor_z, CHUNK_SIZE);
    const int32_t anchor_rx = floor_div_i32(anchor_cx, spacing);
    const int32_t anchor_rz = floor_div_i32(anchor_cz, spacing);
    const int32_t region_radius = std::max<int32_t>(
        2,
        static_cast<int32_t>((rc.candidate_chunk_radius + static_cast<uint32_t>(spacing) - 1U) /
                             static_cast<uint32_t>(spacing)) +
            2
    );

    uint32_t accepted = 0U;
    for (int32_t rx = anchor_rx - region_radius; rx <= anchor_rx + region_radius; ++rx) {
        for (int32_t rz = anchor_rz - region_radius; rz <= anchor_rz + region_radius; ++rz) {
            if (region_aabb_outside_radius(rx, rz, spacing, anchor_x, anchor_z, rc.candidate_radius_sq)) {
                continue;
            }
            if (!direct_structure_attempt_within_radius(seed, rc, rx, rz, anchor_x, anchor_z)) {
                continue;
            }
            ++accepted;
            if (accepted >= stop_after) {
                return accepted;
            }
        }
    }
    return accepted;
}

static SCANNER_FORCE_INLINE SeedStatus evaluate_query_seed_stage0_prefilter(
    uint64_t seed,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    const uint32_t *stage0_constraint_order,
    uint32_t stage0_constraint_count
) {
    if (stage0_constraint_count == 0U || constraints == nullptr || stage0_constraint_order == nullptr) {
        return SeedStatus::valid;
    }

    for (uint32_t ord = 0; ord < stage0_constraint_count; ++ord) {
        const uint32_t idx = stage0_constraint_order[ord];
        if (idx == UINT32_MAX) {
            return SeedStatus::reject;
        }
        const NativeQueryConstraintDesc &rc = constraints[idx];

        int32_t anchor_x = 0;
        int32_t anchor_z = 0;
        if (rc.anchor_type == SCAN_ANCHOR_FIXED) {
            anchor_x = rc.anchor_x;
            anchor_z = rc.anchor_z;
        }

        const uint32_t required = std::max<uint32_t>(1U, rc.min_required);
        uint32_t found = 0U;
        if (rc.prefilter_supported != 0U) {
            found = count_constraint_stage0_region_list_candidates(
                seed,
                regions,
                region_count,
                rc,
                anchor_x,
                anchor_z,
                required
            );
        } else {
            found = count_constraint_stage0_direct_candidates(seed, rc, anchor_x, anchor_z, required);
        }
        if (found < required) {
            return SeedStatus::reject;
        }
    }
    return SeedStatus::valid;
}

template <typename EmitFn>
static SCANNER_FORCE_INLINE uint32_t collect_constraint_candidates_nonstrict_region_list_emit(
    const CubiApi &api,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc &rc,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t target_cap,
    bool count_cap_prune,
    void *g_world,
    EmitFn &&emit
) {
    (void)api;
    (void)mc_version;
    (void)g_world;
    QueryScratch::CandidateDedupe &dedupe = scratch.dedupe;
    uint32_t cap_pruned = 0;
    if (rc.region_count == 0U) {
        return cap_pruned;
    }

    const uint32_t start = rc.region_start;
    const uint32_t end = std::min<uint32_t>(region_count, start + rc.region_count);
    uint32_t accepted = 0;
    for (uint32_t i = start; i < end; ++i) {
        const NativeRegionTerm &region = regions[i];
        if (constraint_region_outside_radius(rc, region, anchor_x, anchor_z)) {
            continue;
        }
        int32_t x = 0;
        int32_t z = 0;
        if (!structure_attempt_within_radius(
                seed,
                region,
                anchor_x,
                anchor_z,
                rc.candidate_radius_sq,
                &x,
                &z)) {
            continue;
        }

        const uint64_t packed = pack_xz_i32(x, z);
        if (!dedupe.insert(packed)) {
            continue;
        }
        emit(packed);
        ++accepted;
        if (accepted >= target_cap) {
            if (count_cap_prune) {
                ++cap_pruned;
            }
            break;
        }
    }
    return cap_pruned;
}

template <typename EmitFn>
static SCANNER_FORCE_INLINE uint32_t collect_constraint_candidates_strict_region_list_emit(
    const CubiApi &api,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc &rc,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t target_cap,
    bool count_cap_prune,
    void *g_world,
    EmitFn &&emit
) {
    QueryScratch::CandidateDedupe &dedupe = scratch.dedupe;
    uint32_t cap_pruned = 0;
    if (rc.region_count == 0U) {
        return cap_pruned;
    }
    if (g_world == nullptr || rc.struct_id < 0) {
        return cap_pruned;
    }

    const uint32_t start = rc.region_start;
    const uint32_t end = std::min<uint32_t>(region_count, start + rc.region_count);
    uint32_t accepted = 0;
    for (uint32_t i = start; i < end; ++i) {
        const NativeRegionTerm &region = regions[i];
        if (constraint_region_outside_radius(rc, region, anchor_x, anchor_z)) {
            debug_validate_region_prune_rejection(
                "strict_region_list",
                api,
                seed,
                mc_version,
                rc,
                region.rx,
                region.rz,
                anchor_x,
                anchor_z,
                g_world
            );
            continue;
        }
        if (rc.exact_attempt_prefilter != 0U &&
            !structure_attempt_within_radius(seed, region, anchor_x, anchor_z, rc.candidate_radius_sq)) {
            debug_validate_attempt_prefilter_rejection(
                "strict_region_list",
                api,
                seed,
                mc_version,
                rc,
                region.rx,
                region.rz,
                anchor_x,
                anchor_z,
                g_world
            );
            continue;
        }
        CPos pos{};
        if (!cached_get_structure_pos_direct(
                api,
                scratch,
                rc.struct_id,
                rc.dimension,
                mc_version,
                seed,
                region.rx,
                region.rz,
                pos)) {
            continue;
        }
        const int32_t x = pos.x;
        const int32_t z = pos.z;
        if (structure_has_viability_check(rc.struct_id) &&
            !cached_is_viable_structure_pos_direct(
                api,
                scratch,
                rc.struct_id,
                rc.dimension,
                mc_version,
                seed,
                g_world,
                region.rx,
                region.rz,
                x,
                z,
                rc.viability_flags)) {
            continue;
        }
        if (dist2_i64(x, z, anchor_x, anchor_z) > rc.candidate_radius_sq) {
            continue;
        }

        const uint64_t packed = pack_xz_i32(x, z);
        if (!dedupe.insert(packed)) {
            continue;
        }
        emit(packed);
        ++accepted;
        if (accepted >= target_cap) {
            if (count_cap_prune) {
                ++cap_pruned;
            }
            break;
        }
    }
    return cap_pruned;
}

template <typename EmitFn>
static SCANNER_FORCE_INLINE uint32_t collect_constraint_candidates_for_mode_emit(
    const CubiApi &api,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc &rc,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t target_cap,
    bool count_cap_prune,
    void *g_world,
    bool force_strict_validation,
    EmitFn &&emit
) {
    if (rc.is_stronghold != 0U) {
        return collect_constraint_candidates_stronghold_emit(
            api,
            scratch,
            seed,
            mc_version,
            regions,
            region_count,
            rc,
            anchor_x,
            anchor_z,
            target_cap,
            count_cap_prune,
            g_world,
            std::forward<EmitFn>(emit)
        );
    }

    if (force_strict_validation || rc.mode_strict != 0U) {
        if (rc.prefilter_supported != 0U) {
            return collect_constraint_candidates_strict_region_list_emit(
                api,
                scratch,
                seed,
                mc_version,
                regions,
                region_count,
                rc,
                anchor_x,
                anchor_z,
                target_cap,
                count_cap_prune,
                g_world,
                std::forward<EmitFn>(emit)
            );
        }
        return collect_constraint_candidates_direct_no_prefilter_emit(
            api,
            scratch,
            seed,
            mc_version,
            regions,
            region_count,
            rc,
            anchor_x,
            anchor_z,
            target_cap,
            count_cap_prune,
            true,
            g_world,
            std::forward<EmitFn>(emit)
        );
    }

    if (rc.prefilter_supported != 0U) {
        return collect_constraint_candidates_nonstrict_region_list_emit(
            api,
            scratch,
            seed,
            mc_version,
            regions,
            region_count,
            rc,
            anchor_x,
            anchor_z,
            target_cap,
            count_cap_prune,
            g_world,
            std::forward<EmitFn>(emit)
        );
    }

    return collect_constraint_candidates_direct_no_prefilter_emit(
        api,
        scratch,
        seed,
        mc_version,
        regions,
        region_count,
        rc,
        anchor_x,
        anchor_z,
        target_cap,
        count_cap_prune,
        false,
        g_world,
        std::forward<EmitFn>(emit)
    );
}

static SCANNER_FORCE_INLINE uint32_t collect_constraint_candidates_packed(
    const CubiApi &api,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc &rc,
    int32_t anchor_x,
    int32_t anchor_z,
    std::vector<uint64_t> &out_points,
    uint32_t target_cap,
    void *g_world,
    bool force_strict_validation
) {
    target_cap = std::max<uint32_t>(1U, std::min<uint32_t>(target_cap, rc.candidate_cap));
    const bool count_cap_prune = target_cap >= rc.candidate_cap;
    scratch.dedupe.prepare(target_cap);
    return collect_constraint_candidates_for_mode_emit(
        api,
        scratch,
        seed,
        mc_version,
        regions,
        region_count,
        rc,
        anchor_x,
        anchor_z,
        target_cap,
        count_cap_prune,
        g_world,
        force_strict_validation,
        [&](uint64_t packed) { out_points.push_back(packed); }
    );
}

struct ConstraintEvalStats {
    uint64_t evaluation_count = 0ULL;
    uint64_t reject_count = 0ULL;
    uint64_t accept_count = 0ULL;
};

template <bool TrackStats>
static SeedStatus evaluate_query_seed_fast_native_impl(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const std::vector<QueryBiomePrepared> &biome_filters,
    const uint32_t *constraint_eval_order,
    const uint32_t *target_caps,
    const uint8_t *constraint_store_mask,
    bool direct_api_needed,
    bool generator_needed,
    uint32_t &cap_pruned_out,
    ConstraintEvalStats *constraint_stats,
    uint32_t constraint_stats_count
) {
    cap_pruned_out = 0;
    scratch.reset();
    const bool biome_needed = !biome_filters.empty();
    const bool debug_prefilter = prefilter_debug_enabled();
    (void)generator_needed;

    if (direct_api_needed && !api.direct_ready) {
        return SeedStatus::reject;
    }
    void *g_world = nullptr;
    auto ensure_world = [&](int32_t dimension) -> void * {
        g_world = ensure_generator(api, st, mc_version, seed, dimension);
        return g_world;
    };

    if (constraint_count > 0U && constraints == nullptr) {
        return SeedStatus::reject;
    }

    bool early_biome_rejected = false;
    bool early_biome_checked = false;
    bool early_spawn_area_rejected = false;
    uint32_t early_biome_reject_index = UINT32_MAX;
    int32_t early_biome_point_x = 0;
    int32_t early_biome_point_z = 0;
    SeedStatus late_status = SeedStatus::valid;
    auto run_early_exact_biome_prefilters = [&]() -> SeedStatus {
        if (!biome_needed || early_biome_checked) {
            return SeedStatus::valid;
        }
        early_biome_checked = true;
        for (uint32_t bf_idx = 0; bf_idx < biome_filters.size(); ++bf_idx) {
            const QueryBiomePrepared &bf = biome_filters[bf_idx];
            if (!biome_filter_is_cheap_exact_early_anchor(bf)) {
                continue;
            }
            int32_t point_x = 0;
            int32_t point_z = 0;
            if (!resolve_exact_early_biome_anchor_point(
                    api,
                    st,
                    scratch,
                    seed,
                    mc_version,
                    g_world,
                    bf.desc,
                    point_x,
                    point_z)) {
                if (!debug_prefilter) {
                    return SeedStatus::biome_reject;
                }
                early_biome_rejected = true;
                early_biome_reject_index = bf_idx;
                break;
            }
            if (query_biome_filter_matches_at_point(
                    api,
                    st,
                    scratch,
                    seed,
                    mc_version,
                    g_world,
                    bf,
                    point_x,
                    point_z)) {
                continue;
            }
            if (!debug_prefilter) {
                return SeedStatus::biome_reject;
            }
            early_biome_rejected = true;
            early_biome_reject_index = bf_idx;
            early_biome_point_x = point_x;
            early_biome_point_z = point_z;
            break;
        }
        return SeedStatus::valid;
    };
    bool early_spawn_area_checked = false;
    auto run_early_spawn_area_biome_prefilters = [&]() -> SeedStatus {
        if (!biome_needed || early_spawn_area_checked) {
            return SeedStatus::valid;
        }
        early_spawn_area_checked = true;
        for (uint32_t bf_idx = 0; bf_idx < biome_filters.size(); ++bf_idx) {
            const QueryBiomePrepared &bf = biome_filters[bf_idx];
            if (bf.desc.point_type != SCAN_ANCHOR_SPAWN || bf.desc.dimension != DIM_OVERWORLD) {
                continue;
            }
            const int32_t spawn_search_radius = spawn_area_prefilter_search_radius(mc_version);
            if (spawn_search_radius <= 0) {
                continue;
            }
            int32_t estimate_x = 0;
            int32_t estimate_z = 0;
            if (!query_get_spawn_estimate(api, st, mc_version, seed, scratch, g_world, estimate_x, estimate_z)) {
                continue;
            }
            // Reject only when the requested biome is absent from every block position
            // that the final server spawn search could still choose after estimateSpawn().
            const std::vector<BiomeOffset> &spawn_area_offsets = build_biome_offsets_cached(
                std::max<int32_t>(0, bf.desc.radius) + spawn_search_radius,
                SPAWN_AREA_PREFILTER_STEP
            );
            if (query_biome_matches_with_offsets_at_point(
                    api,
                    st,
                    scratch,
                    seed,
                    mc_version,
                    g_world,
                    bf.desc.dimension,
                    bf.desc.y,
                    spawn_area_offsets,
                    bf.allowed_sorted,
                    estimate_x,
                    estimate_z)) {
                continue;
            }
            if (!debug_prefilter) {
                return SeedStatus::biome_reject;
            }
            early_biome_rejected = true;
            early_spawn_area_rejected = true;
            early_biome_reject_index = bf_idx;
            break;
        }
        return SeedStatus::valid;
    };
    auto run_early_biome_prefilters = [&]() -> SeedStatus {
        const SeedStatus exact_status = run_early_exact_biome_prefilters();
        if (exact_status != SeedStatus::valid) {
            return exact_status;
        }
        return run_early_spawn_area_biome_prefilters();
    };

    const SeedStatus early_fixed_biome_status = run_early_exact_biome_prefilters();
    if (early_fixed_biome_status != SeedStatus::valid) {
        return early_fixed_biome_status;
    }

    for (uint32_t ord = 0; ord < constraint_count; ++ord) {
        const uint32_t idx = constraint_eval_order == nullptr ? ord : constraint_eval_order[ord];
        if (idx >= constraint_count) {
            return SeedStatus::reject;
        }
        const NativeQueryConstraintDesc &rc = constraints[idx];
        if (biome_needed && !early_biome_checked && constraint_requires_exact_generator(rc)) {
            const SeedStatus prefilter_status = run_early_biome_prefilters();
            if (prefilter_status != SeedStatus::valid) {
                return prefilter_status;
            }
        }
        ConstraintEvalStats *step_stats = nullptr;
        if constexpr (TrackStats) {
            if (constraint_stats != nullptr && idx < constraint_stats_count) {
                step_stats = &constraint_stats[idx];
                ++step_stats->evaluation_count;
            }
        }
        const uint32_t min_required = std::max<uint32_t>(1U, rc.min_required);
        uint32_t target_cap =
            target_caps == nullptr ? std::max<uint32_t>(min_required, rc.candidate_cap) : target_caps[idx];
        target_cap = std::max<uint32_t>(1U, std::min<uint32_t>(target_cap, rc.candidate_cap));
        const uint32_t stop_threshold = rc.quad_max_span == 0U ? min_required : target_cap;
        const bool store_matches = constraint_store_mask != nullptr && constraint_store_mask[idx] != 0U;
        const bool keep_match_points = store_matches || rc.quad_max_span > 0U;

        std::vector<uint64_t> &constraint_matches = scratch.matches_by_constraint[idx];
        constraint_matches.clear();
        scratch.constraint_seen_dedupe.prepare(stop_threshold);
        uint32_t unique_match_count = 0U;
        bool generator_failed = false;

        auto mark_reject = [&](SeedStatus s) {
            if constexpr (TrackStats) {
                if (step_stats != nullptr) {
                    ++step_stats->reject_count;
                }
            }
            return s;
        };

        auto process_anchor = [&](int32_t ax, int32_t az) {
            if (unique_match_count >= stop_threshold) {
                return true;
            }
            const uint32_t remaining = stop_threshold - unique_match_count;
            if (remaining == 0U) {
                return true;
            }
            const uint32_t collect_cap = std::min<uint32_t>(target_cap, remaining);
            const bool count_cap_prune = collect_cap >= rc.candidate_cap;

            void *collect_world = g_world;
            if (constraint_requires_exact_generator(rc)) {
                collect_world = ensure_world(rc.dimension);
                if (collect_world == nullptr) {
                    generator_failed = true;
                    return true;
                }
            }
            scratch.dedupe.prepare(collect_cap);
            cap_pruned_out += collect_constraint_candidates_for_mode_emit(
                api,
                scratch,
                seed,
                mc_version,
                regions,
                region_count,
                rc,
                ax,
                az,
                collect_cap,
                count_cap_prune,
                collect_world,
                false,
                [&](uint64_t packed) {
                    if (scratch.constraint_seen_dedupe.insert(packed)) {
                        ++unique_match_count;
                        if (keep_match_points) {
                            constraint_matches.push_back(packed);
                        }
                    }
                }
            );
            return unique_match_count >= stop_threshold;
        };

        if (rc.anchor_type == SCAN_ANCHOR_ORIGIN) {
            process_anchor(0, 0);
        } else if (rc.anchor_type == SCAN_ANCHOR_SPAWN) {
            int32_t sx = 0;
            int32_t sz = 0;
            if (!query_get_spawn(api, st, mc_version, seed, scratch, g_world, sx, sz)) {
                late_status = mark_reject(SeedStatus::reject);
                goto constraint_eval_done;
            }
            process_anchor(sx, sz);
        } else if (rc.anchor_type == SCAN_ANCHOR_FIXED) {
            process_anchor(rc.anchor_x, rc.anchor_z);
        } else if (rc.anchor_type == SCAN_ANCHOR_DEP) {
            if (rc.anchor_dep_index < 0 || static_cast<uint32_t>(rc.anchor_dep_index) >= constraint_count) {
                late_status = mark_reject(SeedStatus::reject);
                goto constraint_eval_done;
            }
            const std::vector<uint64_t> &deps =
                scratch.matches_by_constraint[static_cast<uint32_t>(rc.anchor_dep_index)];
            if (deps.empty()) {
                late_status = mark_reject(SeedStatus::reject);
                goto constraint_eval_done;
            }
            for (uint64_t packed : deps) {
                if (process_anchor(unpack_x_from_packed(packed), unpack_z_from_packed(packed))) {
                    break;
                }
            }
        } else {
            late_status = mark_reject(SeedStatus::reject);
            goto constraint_eval_done;
        }
        if (generator_failed) {
            late_status = mark_reject(SeedStatus::reject);
            goto constraint_eval_done;
        }

        if (unique_match_count < min_required) {
            late_status = mark_reject(SeedStatus::reject);
            goto constraint_eval_done;
        }
        if (rc.quad_max_span > 0U &&
            !has_compact_group_within_span(constraint_matches, min_required, rc.quad_max_span)) {
            late_status = mark_reject(SeedStatus::reject);
            goto constraint_eval_done;
        }
        if constexpr (TrackStats) {
            if (step_stats != nullptr) {
                ++step_stats->accept_count;
            }
        }
    }
constraint_eval_done:
    if (late_status != SeedStatus::valid) {
        if (early_biome_rejected) {
            return SeedStatus::biome_reject;
        }
        return late_status;
    }

    SeedStatus biome_status = SeedStatus::valid;
    for (uint32_t bf_idx = 0; bf_idx < biome_filters.size(); ++bf_idx) {
        const QueryBiomePrepared &bf = biome_filters[bf_idx];
        const NativeBiomeFilterDesc &desc = bf.desc;
        if (early_biome_checked && !early_biome_rejected && biome_filter_is_cheap_exact_early_anchor(bf)) {
            continue;
        }

        bool passed = false;
        if (desc.point_type == SCAN_ANCHOR_ORIGIN) {
            passed = query_biome_filter_matches_at_point(api, st, scratch, seed, mc_version, g_world, bf, 0, 0);
        } else if (desc.point_type == SCAN_ANCHOR_SPAWN) {
            int32_t sx = 0;
            int32_t sz = 0;
            if (!query_get_spawn(api, st, mc_version, seed, scratch, g_world, sx, sz)) {
                biome_status = SeedStatus::biome_reject;
                break;
            }
            passed = query_biome_filter_matches_at_point(api, st, scratch, seed, mc_version, g_world, bf, sx, sz);
        } else if (desc.point_type == SCAN_ANCHOR_FIXED) {
            passed = query_biome_filter_matches_at_point(
                api,
                st,
                scratch,
                seed,
                mc_version,
                g_world,
                bf,
                desc.point_x,
                desc.point_z
            );
        } else {
            if (desc.point_dep_index < 0 || static_cast<uint32_t>(desc.point_dep_index) >= constraint_count) {
                biome_status = SeedStatus::biome_reject;
                break;
            }
            const std::vector<uint64_t> &deps =
                scratch.matches_by_constraint[static_cast<uint32_t>(desc.point_dep_index)];
            for (uint64_t packed : deps) {
                if (query_biome_filter_matches_at_point(
                        api,
                        st,
                        scratch,
                        seed,
                        mc_version,
                        g_world,
                        bf,
                        unpack_x_from_packed(packed),
                        unpack_z_from_packed(packed))) {
                    passed = true;
                    break;
                }
            }
        }
        if (!passed) {
            biome_status = SeedStatus::biome_reject;
            break;
        }
    }
    if (early_biome_rejected) {
        if (biome_status == SeedStatus::valid && early_biome_reject_index < biome_filters.size()) {
            if (early_spawn_area_rejected) {
                debug_log_spawn_area_prefilter_false_negative(
                    seed,
                    early_biome_reject_index,
                    biome_filters[early_biome_reject_index].desc,
                    spawn_area_prefilter_search_radius(mc_version)
                );
            } else {
                debug_log_biome_prefilter_false_negative(
                    seed,
                    early_biome_reject_index,
                    biome_filters[early_biome_reject_index].desc,
                    early_biome_point_x,
                    early_biome_point_z
                );
            }
        }
        return SeedStatus::biome_reject;
    }
    return biome_status;
}

static SeedStatus evaluate_query_seed_fast_native(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const std::vector<QueryBiomePrepared> &biome_filters,
    const uint32_t *constraint_eval_order,
    const uint32_t *target_caps,
    const uint8_t *constraint_store_mask,
    bool direct_api_needed,
    bool generator_needed,
    uint32_t &cap_pruned_out
) {
    return evaluate_query_seed_fast_native_impl<false>(
        api,
        st,
        scratch,
        seed,
        mc_version,
        regions,
        region_count,
        constraints,
        constraint_count,
        biome_filters,
        constraint_eval_order,
        target_caps,
        constraint_store_mask,
        direct_api_needed,
        generator_needed,
        cap_pruned_out,
        nullptr,
        0U
    );
}

static SeedStatus evaluate_query_seed_fast_native_warmup(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const std::vector<QueryBiomePrepared> &biome_filters,
    const uint32_t *constraint_eval_order,
    const uint32_t *target_caps,
    const uint8_t *constraint_store_mask,
    bool direct_api_needed,
    bool generator_needed,
    uint32_t &cap_pruned_out,
    ConstraintEvalStats *constraint_stats,
    uint32_t constraint_stats_count
) {
    return evaluate_query_seed_fast_native_impl<true>(
        api,
        st,
        scratch,
        seed,
        mc_version,
        regions,
        region_count,
        constraints,
        constraint_count,
        biome_filters,
        constraint_eval_order,
        target_caps,
        constraint_store_mask,
        direct_api_needed,
        generator_needed,
        cap_pruned_out,
        constraint_stats,
        constraint_stats_count
    );
}

static SeedStatus collect_query_details_native(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const std::vector<QueryBiomePrepared> &biome_filters,
    bool direct_api_needed,
    bool generator_needed,
    std::vector<std::vector<DetailPoint>> &detail_by_constraint,
    std::vector<NativeQueryBiomeOut> &biome_records,
    uint32_t &cap_pruned_out
) {
    cap_pruned_out = 0;
    scratch.reset();
    detail_by_constraint.assign(constraint_count, std::vector<DetailPoint>{});
    biome_records.clear();
    (void)generator_needed;

    if (direct_api_needed && !api.direct_ready) {
        return SeedStatus::reject;
    }
    void *g_world = nullptr;
    auto ensure_world = [&](int32_t dimension) -> void * {
        g_world = ensure_generator(api, st, mc_version, seed, dimension);
        return g_world;
    };

    std::vector<uint64_t> anchor_points;

    for (uint32_t idx = 0; idx < constraint_count; ++idx) {
        const NativeQueryConstraintDesc &rc = constraints[idx];
        const uint32_t min_required = std::max<uint32_t>(1U, rc.min_required);
        std::vector<uint64_t> &constraint_matches = scratch.matches_by_constraint[idx];
        std::vector<DetailPoint> &detail_matches = detail_by_constraint[idx];
        constraint_matches.clear();
        detail_matches.clear();
        scratch.constraint_seen_dedupe.prepare(rc.candidate_cap);
        if (anchor_points.capacity() < static_cast<size_t>(rc.candidate_cap)) {
            anchor_points.reserve(static_cast<size_t>(rc.candidate_cap));
        }

        std::vector<DetailKey> detail_dedupe;
        detail_dedupe.reserve(static_cast<size_t>(rc.candidate_cap));
        bool generator_failed = false;

        auto process_anchor = [&](int32_t ax, int32_t az) {
            void *collect_world = g_world;
            if (constraint_requires_exact_generator(rc)) {
                collect_world = ensure_world(rc.dimension);
                if (collect_world == nullptr) {
                    generator_failed = true;
                    return;
                }
            }
            anchor_points.clear();
            cap_pruned_out += collect_constraint_candidates_packed(
                api,
                scratch,
                seed,
                mc_version,
                regions,
                region_count,
                rc,
                ax,
                az,
                anchor_points,
                rc.candidate_cap,
                collect_world,
                false
            );
            for (uint64_t packed : anchor_points) {
                if (scratch.constraint_seen_dedupe.insert(packed)) {
                    constraint_matches.push_back(packed);
                }
                DetailPoint dp{
                    unpack_x_from_packed(packed),
                    unpack_z_from_packed(packed),
                    ax,
                    az,
                };
                DetailKey key{dp.x, dp.z, dp.anchor_x, dp.anchor_z};
                if (insert_unique_small_detail(detail_dedupe, key)) {
                    detail_matches.push_back(dp);
                }
            }
        };

        if (rc.anchor_type == SCAN_ANCHOR_ORIGIN) {
            process_anchor(0, 0);
        } else if (rc.anchor_type == SCAN_ANCHOR_SPAWN) {
            int32_t sx = 0;
            int32_t sz = 0;
            if (!query_get_spawn(api, st, mc_version, seed, scratch, g_world, sx, sz)) {
                return SeedStatus::reject;
            }
            process_anchor(sx, sz);
        } else if (rc.anchor_type == SCAN_ANCHOR_FIXED) {
            process_anchor(rc.anchor_x, rc.anchor_z);
        } else {
            if (rc.anchor_dep_index < 0 || static_cast<uint32_t>(rc.anchor_dep_index) >= idx) {
                return SeedStatus::reject;
            }
            const std::vector<uint64_t> &deps =
                scratch.matches_by_constraint[static_cast<uint32_t>(rc.anchor_dep_index)];
            if (deps.empty()) {
                return SeedStatus::reject;
            }
            for (uint64_t packed : deps) {
                process_anchor(unpack_x_from_packed(packed), unpack_z_from_packed(packed));
            }
        }
        if (generator_failed) {
            return SeedStatus::reject;
        }

        if (constraint_matches.size() < static_cast<size_t>(min_required)) {
            return SeedStatus::reject;
        }
        if (rc.quad_max_span > 0U &&
            !has_compact_group_within_span(constraint_matches, min_required, rc.quad_max_span)) {
            return SeedStatus::reject;
        }
    }

    biome_records.reserve(biome_filters.size());
    for (uint32_t bf_idx = 0; bf_idx < biome_filters.size(); ++bf_idx) {
        const QueryBiomePrepared &bf = biome_filters[bf_idx];
        const NativeBiomeFilterDesc &desc = bf.desc;

        auto try_point = [&](int32_t px, int32_t pz, NativeQueryBiomeOut &out) {
            int32_t mx = 0;
            int32_t mz = 0;
            int32_t bid = -1;
            uint64_t d2 = 0;
            if (bf.offsets == nullptr) {
                return false;
            }
            void *biome_world = ensure_world(desc.dimension);
            if (biome_world == nullptr) {
                return false;
            }
            for (const BiomeOffset &o : *bf.offsets) {
                const int32_t sx = px + o.dx;
                const int32_t sz = pz + o.dz;
                const int biome_id = cached_query_biome_at(api, scratch, biome_world, desc.dimension, sx, desc.y, sz);
                if (!contains_biome_id(bf.allowed_sorted, biome_id)) {
                    continue;
                }
                mx = sx;
                mz = sz;
                bid = biome_id;
                d2 = o.d2;
                break;
            }
            if (bid < 0) {
                return false;
            }
            out.filter_index = bf_idx;
            out.x = mx;
            out.y = desc.y;
            out.z = mz;
            out.radius = desc.radius;
            out.dist2 = d2;
            out.biome_id = bid;
            return true;
        };

        bool passed = false;
        NativeQueryBiomeOut record{};
        if (desc.point_type == SCAN_ANCHOR_ORIGIN) {
            passed = try_point(0, 0, record);
        } else if (desc.point_type == SCAN_ANCHOR_SPAWN) {
            int32_t sx = 0;
            int32_t sz = 0;
            if (!query_get_spawn(api, st, mc_version, seed, scratch, g_world, sx, sz)) {
                return SeedStatus::biome_reject;
            }
            passed = try_point(sx, sz, record);
        } else if (desc.point_type == SCAN_ANCHOR_FIXED) {
            passed = try_point(desc.point_x, desc.point_z, record);
        } else {
            if (desc.point_dep_index < 0 || static_cast<uint32_t>(desc.point_dep_index) >= constraint_count) {
                return SeedStatus::biome_reject;
            }
            const std::vector<uint64_t> &deps =
                scratch.matches_by_constraint[static_cast<uint32_t>(desc.point_dep_index)];
            for (uint64_t packed : deps) {
                if (try_point(unpack_x_from_packed(packed), unpack_z_from_packed(packed), record)) {
                    passed = true;
                    break;
                }
            }
        }
        if (!passed) {
            return SeedStatus::biome_reject;
        }
        biome_records.push_back(record);
    }

    return SeedStatus::valid;
}

struct alignas(64) BatchTaskResult {
    std::vector<uint64_t> valid;
    uint32_t mismatch = 0;
    uint32_t biome_reject = 0;
    uint32_t cap_pruned = 0;
};

struct alignas(64) BatchChunkResult {
    std::vector<uint64_t> valid;
    uint32_t mismatch = 0U;
    uint32_t biome_reject = 0U;
    uint32_t cap_pruned = 0U;
};

static std::vector<uint8_t> build_constraint_dependency_mask(
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count
) {
    std::vector<uint8_t> has_dependents(constraint_count, 0U);
    if (constraint_count == 0U || constraints == nullptr) {
        return has_dependents;
    }

    for (uint32_t i = 0; i < constraint_count; ++i) {
        const NativeQueryConstraintDesc &c = constraints[i];
        if (c.anchor_type != SCAN_ANCHOR_DEP || c.anchor_dep_index < 0) {
            continue;
        }
        const uint32_t dep_idx = static_cast<uint32_t>(c.anchor_dep_index);
        if (dep_idx < constraint_count) {
            has_dependents[dep_idx] = 1U;
        }
    }
    for (uint32_t i = 0; i < biome_filter_count; ++i) {
        if (biome_filters == nullptr) {
            break;
        }
        const NativeBiomeFilterDesc &bf = biome_filters[i];
        if (bf.point_type != SCAN_ANCHOR_DEP || bf.point_dep_index < 0) {
            continue;
        }
        const uint32_t dep_idx = static_cast<uint32_t>(bf.point_dep_index);
        if (dep_idx < constraint_count) {
            has_dependents[dep_idx] = 1U;
        }
    }
    return has_dependents;
}

static std::vector<uint32_t> build_fast_target_caps(
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count
) {
    std::vector<uint32_t> caps;
    caps.reserve(constraint_count);
    if (constraint_count == 0U || constraints == nullptr) {
        return caps;
    }

    const std::vector<uint8_t> has_dependents =
        build_constraint_dependency_mask(constraints, constraint_count, biome_filters, biome_filter_count);

    for (uint32_t i = 0; i < constraint_count; ++i) {
        const uint32_t min_required = std::max<uint32_t>(1U, constraints[i].min_required);
        const uint32_t base_cap = std::max<uint32_t>(min_required, constraints[i].candidate_cap);
        caps.push_back(has_dependents[i] != 0U ? base_cap : min_required);
    }
    return caps;
}

static SCANNER_FORCE_INLINE bool strict_constraint_better_for_eval(
    const NativeQueryConstraintDesc &lhs,
    uint32_t lhs_idx,
    const NativeQueryConstraintDesc &rhs,
    uint32_t rhs_idx
) {
    if (lhs.is_stronghold != rhs.is_stronghold) {
        return lhs.is_stronghold < rhs.is_stronghold;
    }
    if (lhs.prefilter_supported != rhs.prefilter_supported) {
        return lhs.prefilter_supported > rhs.prefilter_supported;
    }
    if (lhs.candidate_cap != rhs.candidate_cap) {
        return lhs.candidate_cap < rhs.candidate_cap;
    }
    if (lhs.region_count != rhs.region_count) {
        return lhs.region_count < rhs.region_count;
    }
    if (lhs.candidate_chunk_radius != rhs.candidate_chunk_radius) {
        return lhs.candidate_chunk_radius < rhs.candidate_chunk_radius;
    }
    if (lhs.min_required != rhs.min_required) {
        return lhs.min_required > rhs.min_required;
    }
    const bool lhs_quad = lhs.quad_max_span > 0U;
    const bool rhs_quad = rhs.quad_max_span > 0U;
    if (lhs_quad != rhs_quad) {
        return lhs_quad;
    }
    if (lhs.quad_max_span != rhs.quad_max_span) {
        return lhs.quad_max_span < rhs.quad_max_span;
    }
    if (lhs.candidate_radius_sq != rhs.candidate_radius_sq) {
        return lhs.candidate_radius_sq < rhs.candidate_radius_sq;
    }
    return lhs_idx < rhs_idx;
}

static SCANNER_FORCE_INLINE bool reject_ratio_higher(
    const ConstraintEvalStats &lhs,
    const ConstraintEvalStats &rhs
) {
    // Cross-multiply to avoid FP overhead and keep deterministic ordering.
    const uint64_t lscore = lhs.reject_count * rhs.evaluation_count;
    const uint64_t rscore = rhs.reject_count * lhs.evaluation_count;
    if (lscore != rscore) {
        return lscore > rscore;
    }
    return lhs.reject_count > rhs.reject_count;
}

static std::vector<uint32_t> build_constraint_eval_order(
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count
) {
    std::vector<uint32_t> out;
    out.reserve(constraint_count);
    if (constraint_count == 0U || constraints == nullptr) {
        return out;
    }

    std::vector<uint8_t> done(constraint_count, 0U);
    for (uint32_t built = 0; built < constraint_count; ++built) {
        uint32_t best_idx = constraint_count;

        for (uint32_t i = 0; i < constraint_count; ++i) {
            if (done[i] != 0U) {
                continue;
            }
            const NativeQueryConstraintDesc &c = constraints[i];
            if (c.anchor_type == SCAN_ANCHOR_DEP) {
                if (c.anchor_dep_index < 0) {
                    continue;
                }
                const uint32_t dep_idx = static_cast<uint32_t>(c.anchor_dep_index);
                if (dep_idx >= constraint_count || done[dep_idx] == 0U) {
                    continue;
                }
            }

            const bool is_strict = c.mode_strict != 0U;
            if (best_idx == constraint_count) {
                best_idx = i;
                continue;
            }
            const NativeQueryConstraintDesc &best = constraints[best_idx];
            const bool best_is_strict = best.mode_strict != 0U;
            if (is_strict != best_is_strict) {
                // Keep non-strict constraints before strict when DAG permits.
                if (!is_strict) {
                    best_idx = i;
                }
                continue;
            }
            if (is_strict) {
                if (strict_constraint_better_for_eval(c, i, best, best_idx)) {
                    best_idx = i;
                }
            } else if (i < best_idx) {
                best_idx = i;
            }
        }

        if (best_idx == constraint_count) {
            break;
        }
        done[best_idx] = 1U;
        out.push_back(best_idx);
    }

    if (out.size() == constraint_count) {
        return out;
    }

    // Fallback for invalid/partial dependency graphs: keep deterministic index order.
    out.clear();
    out.reserve(constraint_count);
    for (uint32_t i = 0; i < constraint_count; ++i) {
        out.push_back(i);
    }
    return out;
}

static std::vector<uint32_t> build_effective_target_caps(
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const uint32_t *fast_target_caps
) {
    std::vector<uint32_t> out;
    out.reserve(constraint_count);
    if (constraint_count == 0U || constraints == nullptr) {
        return out;
    }

    for (uint32_t i = 0; i < constraint_count; ++i) {
        const NativeQueryConstraintDesc &c = constraints[i];
        const uint32_t min_required = std::max<uint32_t>(1U, c.min_required);
        const uint32_t requested_cap =
            fast_target_caps == nullptr
                ? std::max<uint32_t>(min_required, c.candidate_cap)
                : std::max<uint32_t>(min_required, fast_target_caps[i]);
        const uint32_t cap = std::max<uint32_t>(1U, std::min<uint32_t>(requested_cap, c.candidate_cap));
        out.push_back(cap);
    }
    return out;
}

static std::vector<uint32_t> build_constraint_eval_order_by_selectivity(
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const std::vector<uint32_t> &base_order,
    const std::vector<ConstraintEvalStats> &stats,
    uint64_t min_eval_samples
) {
    if (constraint_count == 0U || constraints == nullptr || base_order.size() != constraint_count ||
        stats.size() < constraint_count) {
        return base_order;
    }

    std::vector<uint32_t> base_pos(constraint_count, constraint_count);
    for (uint32_t pos = 0; pos < constraint_count; ++pos) {
        const uint32_t idx = base_order[pos];
        if (idx >= constraint_count) {
            return base_order;
        }
        base_pos[idx] = pos;
    }

    std::vector<uint32_t> out;
    out.reserve(constraint_count);

    std::vector<uint8_t> done(constraint_count, 0U);
    for (uint32_t built = 0; built < constraint_count; ++built) {
        uint32_t best_idx = constraint_count;
        for (uint32_t i = 0; i < constraint_count; ++i) {
            if (done[i] != 0U) {
                continue;
            }
            const NativeQueryConstraintDesc &c = constraints[i];
            if (c.anchor_type == SCAN_ANCHOR_DEP) {
                if (c.anchor_dep_index < 0) {
                    continue;
                }
                const uint32_t dep_idx = static_cast<uint32_t>(c.anchor_dep_index);
                if (dep_idx >= constraint_count || done[dep_idx] == 0U) {
                    continue;
                }
            }

            if (best_idx == constraint_count) {
                best_idx = i;
                continue;
            }

            const NativeQueryConstraintDesc &best = constraints[best_idx];
            const bool is_strict = c.mode_strict != 0U;
            const bool best_is_strict = best.mode_strict != 0U;
            if (is_strict != best_is_strict) {
                if (!is_strict) {
                    best_idx = i;
                }
                continue;
            }

            if (!is_strict) {
                if (base_pos[i] < base_pos[best_idx]) {
                    best_idx = i;
                }
                continue;
            }

            const bool has_samples = stats[i].evaluation_count >= min_eval_samples;
            const bool best_has_samples = stats[best_idx].evaluation_count >= min_eval_samples;
            if (has_samples != best_has_samples) {
                if (has_samples) {
                    best_idx = i;
                }
                continue;
            }
            if (has_samples &&
                reject_ratio_higher(stats[i], stats[best_idx])) {
                best_idx = i;
                continue;
            }
            if (base_pos[i] < base_pos[best_idx]) {
                best_idx = i;
            }
        }

        if (best_idx == constraint_count) {
            break;
        }
        done[best_idx] = 1U;
        out.push_back(best_idx);
    }

    return out.size() == constraint_count ? out : base_order;
}

static int prepare_query_biome_filters(
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count,
    std::vector<QueryBiomePrepared> &prepared_filters
) {
    prepared_filters.clear();
    prepared_filters.reserve(biome_filter_count);
    for (uint32_t i = 0; i < biome_filter_count; ++i) {
        const NativeBiomeFilterDesc &bf = biome_filters[i];
        if (biome_allowed_ids == nullptr) {
            return -1;
        }
        if (bf.allowed_start > biome_allowed_count || bf.allowed_start + bf.allowed_count > biome_allowed_count) {
            return -1;
        }
        QueryBiomePrepared prep{};
        prep.desc = bf;
        prep.allowed_sorted = sorted_unique_ids(biome_allowed_ids + bf.allowed_start, bf.allowed_count);
        if (prep.allowed_sorted.empty()) {
            return -1;
        }
        prep.offsets = &build_biome_offsets_cached(
            std::max<int32_t>(0, bf.radius),
            static_cast<int32_t>(bf.sample_step)
        );
        prepared_filters.push_back(std::move(prep));
    }
    return 0;
}

static int validate_query_descriptors(
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count
) {
    if (constraint_count > 0U && constraints == nullptr) {
        return -1;
    }
    if ((region_count > 0U && regions == nullptr) || (biome_filter_count > 0U && biome_filters == nullptr)) {
        return -1;
    }

    for (uint32_t i = 0; i < constraint_count; ++i) {
        const NativeQueryConstraintDesc &c = constraints[i];
        if (!valid_scan_dimension(c.dimension)) {
            return -1;
        }
        if (c.region_start > region_count || c.region_start + c.region_count > region_count) {
            return -1;
        }
        if (c.anchor_type != SCAN_ANCHOR_ORIGIN && c.anchor_type != SCAN_ANCHOR_SPAWN &&
            c.anchor_type != SCAN_ANCHOR_DEP && c.anchor_type != SCAN_ANCHOR_FIXED) {
            return -1;
        }
        if (c.anchor_type == SCAN_ANCHOR_DEP &&
            (c.anchor_dep_index < 0 || static_cast<uint32_t>(c.anchor_dep_index) >= i)) {
            return -1;
        }
    }
    for (uint32_t i = 0; i < biome_filter_count; ++i) {
        const NativeBiomeFilterDesc &bf = biome_filters[i];
        if (!valid_scan_dimension(bf.dimension)) {
            return -1;
        }
        if (bf.point_type != SCAN_ANCHOR_ORIGIN && bf.point_type != SCAN_ANCHOR_SPAWN &&
            bf.point_type != SCAN_ANCHOR_DEP && bf.point_type != SCAN_ANCHOR_FIXED) {
            return -1;
        }
        if (bf.point_type == SCAN_ANCHOR_DEP &&
            (bf.point_dep_index < 0 || static_cast<uint32_t>(bf.point_dep_index) >= constraint_count)) {
            return -1;
        }
    }
    return 0;
}

static SCANNER_FORCE_INLINE bool constraint_has_stage_a_exact_anchor_capability(const NativeQueryConstraintDesc &rc) {
    return rc.attempt_anchor_exact_stage0 != 0U && rc.prefilter_supported != 0U && rc.is_stronghold == 0U;
}

static SCANNER_FORCE_INLINE bool stage_a_driver_better(
    const NativeQueryConstraintDesc &lhs,
    uint32_t lhs_idx,
    const NativeQueryConstraintDesc &rhs,
    uint32_t rhs_idx
) {
    const uint32_t lhs_anchor_rank = lhs.anchor_type == SCAN_ANCHOR_ORIGIN ? 0U : 1U;
    const uint32_t rhs_anchor_rank = rhs.anchor_type == SCAN_ANCHOR_ORIGIN ? 0U : 1U;
    if (lhs_anchor_rank != rhs_anchor_rank) {
        return lhs_anchor_rank < rhs_anchor_rank;
    }
    if (lhs.candidate_cap != rhs.candidate_cap) {
        return lhs.candidate_cap < rhs.candidate_cap;
    }
    if (lhs.region_count != rhs.region_count) {
        return lhs.region_count < rhs.region_count;
    }
    if (lhs.min_required != rhs.min_required) {
        return lhs.min_required > rhs.min_required;
    }
    if (lhs.candidate_radius_sq != rhs.candidate_radius_sq) {
        return lhs.candidate_radius_sq < rhs.candidate_radius_sq;
    }
    return lhs_idx < rhs_idx;
}

static uint32_t compiled_biome_group_sample_index(CompiledBiomeGroup &group, const BiomeOffset &offset) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(group.sample_offsets.size()); ++i) {
        const BiomeOffset &existing = group.sample_offsets[i];
        if (existing.dx == offset.dx && existing.dz == offset.dz) {
            return i;
        }
    }
    group.sample_offsets.push_back(offset);
    return static_cast<uint32_t>(group.sample_offsets.size() - 1U);
}

static std::vector<CompiledBiomeGroup> build_compiled_biome_groups(
    const std::vector<QueryBiomePrepared> &prepared_filters
) {
    std::vector<CompiledBiomeGroup> groups;
    groups.reserve(prepared_filters.size());
    for (uint32_t bf_idx = 0; bf_idx < prepared_filters.size(); ++bf_idx) {
        const QueryBiomePrepared &bf = prepared_filters[bf_idx];
        if (bf.offsets == nullptr) {
            continue;
        }

        size_t group_idx = groups.size();
        for (size_t i = 0; i < groups.size(); ++i) {
            const CompiledBiomeGroup &group = groups[i];
            if (group.point_type == bf.desc.point_type && group.point_dep_index == bf.desc.point_dep_index &&
                group.dimension == bf.desc.dimension &&
                group.point_x == bf.desc.point_x && group.point_z == bf.desc.point_z && group.y == bf.desc.y &&
                group.sample_step == bf.desc.sample_step) {
                group_idx = i;
                break;
            }
        }
        if (group_idx == groups.size()) {
            CompiledBiomeGroup group{};
            group.point_type = bf.desc.point_type;
            group.point_dep_index = bf.desc.point_dep_index;
            group.dimension = bf.desc.dimension;
            group.point_x = bf.desc.point_x;
            group.point_z = bf.desc.point_z;
            group.y = bf.desc.y;
            group.sample_step = bf.desc.sample_step;
            group.envelope_radius = std::max<int32_t>(0, bf.desc.radius);
            groups.push_back(std::move(group));
        } else {
            groups[group_idx].envelope_radius =
                std::max<int32_t>(groups[group_idx].envelope_radius, std::max<int32_t>(0, bf.desc.radius));
        }

        CompiledBiomeGroup &group = groups[group_idx];
        CompiledBiomeGroupFilter filter_ref{};
        filter_ref.filter_index = bf_idx;
        filter_ref.sample_indices.reserve(bf.offsets->size());
        for (const BiomeOffset &offset : *bf.offsets) {
            filter_ref.sample_indices.push_back(compiled_biome_group_sample_index(group, offset));
        }
        group.filters.push_back(std::move(filter_ref));
    }
    return groups;
}

static void build_compiled_stage_partitions(CompiledQueryPlan &plan) {
    const uint32_t constraint_count = static_cast<uint32_t>(plan.constraints.size());
    plan.stage_a_exact_anchor_mask.assign(constraint_count, 0U);
    plan.stage_a_available_mask.assign(constraint_count, 0U);
    plan.stage_a_independent.clear();
    plan.stage_a_dependent.clear();
    plan.stage_a_driver_order.clear();
    plan.stage_a_reject_only_order.clear();
    plan.stage_b_exact_structure.clear();
    plan.stage_b_stronghold.clear();

    for (uint32_t idx : plan.base_constraint_eval_order) {
        if (idx >= constraint_count) {
            continue;
        }
        const NativeQueryConstraintDesc &rc = plan.constraints[idx];
        if (rc.is_stronghold != 0U) {
            plan.stage_b_stronghold.push_back(idx);
        } else {
            plan.stage_b_exact_structure.push_back(idx);
        }
        if (!constraint_has_stage_a_exact_anchor_capability(rc)) {
            continue;
        }
        plan.stage_a_exact_anchor_mask[idx] = 1U;
        if (rc.anchor_type == SCAN_ANCHOR_ORIGIN || rc.anchor_type == SCAN_ANCHOR_FIXED) {
            plan.stage_a_independent.push_back(idx);
            plan.stage_a_available_mask[idx] = 1U;
            continue;
        }
        if (rc.anchor_type == SCAN_ANCHOR_DEP && rc.anchor_dep_index >= 0) {
            const uint32_t dep_idx = static_cast<uint32_t>(rc.anchor_dep_index);
            if (dep_idx < constraint_count && plan.stage_a_available_mask[dep_idx] != 0U) {
                plan.stage_a_dependent.push_back(idx);
                plan.stage_a_available_mask[idx] = 1U;
            }
        }
    }

    uint32_t driver_idx = UINT32_MAX;
    for (uint32_t idx : plan.stage_a_independent) {
        if (driver_idx == UINT32_MAX ||
            stage_a_driver_better(plan.constraints[idx], idx, plan.constraints[driver_idx], driver_idx)) {
            driver_idx = idx;
        }
    }
    if (driver_idx != UINT32_MAX) {
        plan.stage_a_driver_order.push_back(driver_idx);
    }
    for (uint32_t idx : plan.base_constraint_eval_order) {
        if (idx >= constraint_count || plan.stage_a_available_mask[idx] == 0U || idx == driver_idx) {
            continue;
        }
        plan.stage_a_driver_order.push_back(idx);
    }
    for (uint32_t idx : plan.base_stage0_constraint_eval_order) {
        if (idx >= constraint_count || plan.stage_a_available_mask[idx] != 0U) {
            continue;
        }
        plan.stage_a_reject_only_order.push_back(idx);
    }
}

static SeedStatus evaluate_query_seed_stage_a_compiled(
    QueryScratch &scratch,
    uint64_t seed,
    const CompiledQueryPlan &plan,
    uint32_t &cap_pruned_out,
    uint64_t *per_constraint_rejects = nullptr
) {
    cap_pruned_out = 0U;
    if (plan.stage_a_driver_order.empty() && plan.stage_a_reject_only_order.empty()) {
        return SeedStatus::valid;
    }

    scratch.reset();
    const CubiApi dummy_api{};
    const NativeRegionTerm *regions = plan.regions.empty() ? nullptr : plan.regions.data();
    const uint32_t region_count = static_cast<uint32_t>(plan.regions.size());
    const NativeQueryConstraintDesc *constraints =
        plan.constraints.empty() ? nullptr : plan.constraints.data();

    for (uint32_t idx : plan.stage_a_driver_order) {
        if (constraints == nullptr || idx >= plan.constraints.size()) {
            return SeedStatus::reject;
        }
        const NativeQueryConstraintDesc &rc = constraints[idx];
        const uint32_t min_required = std::max<uint32_t>(1U, rc.min_required);
        const uint32_t target_cap =
            plan.effective_target_caps.empty()
                ? std::max<uint32_t>(min_required, rc.candidate_cap)
                : plan.effective_target_caps[idx];
        const uint32_t stop_threshold = rc.quad_max_span == 0U ? min_required : target_cap;
        const bool keep_match_points =
            (idx < plan.constraint_store_mask.size() && plan.constraint_store_mask[idx] != 0U) ||
            rc.quad_max_span > 0U;

        std::vector<uint64_t> &constraint_matches = scratch.matches_by_constraint[idx];
        constraint_matches.clear();
        scratch.constraint_seen_dedupe.prepare(stop_threshold);
        uint32_t unique_match_count = 0U;

        auto process_anchor = [&](int32_t ax, int32_t az) {
            if (unique_match_count >= stop_threshold) {
                return true;
            }
            const uint32_t remaining = stop_threshold - unique_match_count;
            if (remaining == 0U) {
                return true;
            }
            const uint32_t collect_cap = std::min<uint32_t>(target_cap, remaining);
            const bool count_cap_prune = collect_cap >= rc.candidate_cap;
            scratch.dedupe.prepare(collect_cap);
            cap_pruned_out += collect_constraint_candidates_nonstrict_region_list_emit(
                dummy_api,
                scratch,
                seed,
                0,
                regions,
                region_count,
                rc,
                ax,
                az,
                collect_cap,
                count_cap_prune,
                nullptr,
                [&](uint64_t packed) {
                    if (!scratch.constraint_seen_dedupe.insert(packed)) {
                        return;
                    }
                    ++unique_match_count;
                    if (keep_match_points) {
                        constraint_matches.push_back(packed);
                    }
                }
            );
            return unique_match_count >= stop_threshold;
        };

        if (rc.anchor_type == SCAN_ANCHOR_ORIGIN) {
            process_anchor(0, 0);
        } else if (rc.anchor_type == SCAN_ANCHOR_FIXED) {
            process_anchor(rc.anchor_x, rc.anchor_z);
        } else if (rc.anchor_type == SCAN_ANCHOR_DEP) {
            if (rc.anchor_dep_index < 0 || static_cast<uint32_t>(rc.anchor_dep_index) >= plan.constraints.size()) {
                if (per_constraint_rejects != nullptr && idx < plan.constraints.size()) {
                    ++per_constraint_rejects[idx];
                }
                return SeedStatus::reject;
            }
            const std::vector<uint64_t> &deps =
                scratch.matches_by_constraint[static_cast<uint32_t>(rc.anchor_dep_index)];
            if (deps.empty()) {
                if (per_constraint_rejects != nullptr && idx < plan.constraints.size()) {
                    ++per_constraint_rejects[idx];
                }
                return SeedStatus::reject;
            }
            for (uint64_t packed : deps) {
                if (process_anchor(unpack_x_from_packed(packed), unpack_z_from_packed(packed))) {
                    break;
                }
            }
        } else {
            if (per_constraint_rejects != nullptr && idx < plan.constraints.size()) {
                ++per_constraint_rejects[idx];
            }
            return SeedStatus::reject;
        }

        if (unique_match_count < min_required) {
            if (per_constraint_rejects != nullptr && idx < plan.constraints.size()) {
                ++per_constraint_rejects[idx];
            }
            return SeedStatus::reject;
        }
        if (rc.quad_max_span > 0U &&
            !has_compact_group_within_span(constraint_matches, min_required, rc.quad_max_span)) {
            if (per_constraint_rejects != nullptr && idx < plan.constraints.size()) {
                ++per_constraint_rejects[idx];
            }
            return SeedStatus::reject;
        }
    }

    for (uint32_t idx : plan.stage_a_reject_only_order) {
        if (constraints == nullptr || idx >= plan.constraints.size()) {
            return SeedStatus::reject;
        }
        const NativeQueryConstraintDesc &rc = constraints[idx];
        int32_t anchor_x = 0;
        int32_t anchor_z = 0;
        if (rc.anchor_type == SCAN_ANCHOR_FIXED) {
            anchor_x = rc.anchor_x;
            anchor_z = rc.anchor_z;
        }

        const uint32_t required = std::max<uint32_t>(1U, rc.min_required);
        uint32_t found = 0U;
        if (rc.prefilter_supported != 0U) {
            found = count_constraint_stage0_region_list_candidates(
                seed,
                regions,
                region_count,
                rc,
                anchor_x,
                anchor_z,
                required
            );
        } else {
            found = count_constraint_stage0_direct_candidates(seed, rc, anchor_x, anchor_z, required);
        }
        if (found < required) {
            if (per_constraint_rejects != nullptr && idx < plan.constraints.size()) {
                ++per_constraint_rejects[idx];
            }
            return SeedStatus::reject;
        }
    }
    return SeedStatus::valid;
}

template <bool TrackStats>
static SeedStatus evaluate_query_seed_stage_b_constraints_compiled(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const CompiledQueryPlan &plan,
    const uint32_t *constraint_eval_order,
    const uint32_t *target_caps,
    uint32_t &cap_pruned_out,
    ConstraintEvalStats *constraint_stats,
    uint32_t constraint_stats_count,
    uint64_t *per_constraint_rejects = nullptr
) {
    cap_pruned_out = 0U;
    scratch.reset();

    if (plan.direct_api_needed && !api.direct_ready) {
        return SeedStatus::reject;
    }

    const NativeRegionTerm *regions = plan.regions.empty() ? nullptr : plan.regions.data();
    const uint32_t region_count = static_cast<uint32_t>(plan.regions.size());
    const NativeQueryConstraintDesc *constraints =
        plan.constraints.empty() ? nullptr : plan.constraints.data();
    const uint32_t constraint_count = static_cast<uint32_t>(plan.constraints.size());

    void *g_world = nullptr;
    auto ensure_world = [&](int32_t dimension) -> void * {
        g_world = ensure_generator(api, st, mc_version, seed, dimension);
        return g_world;
    };

    if (constraint_count > 0U && constraints == nullptr) {
        return SeedStatus::reject;
    }

    for (uint32_t ord = 0; ord < constraint_count; ++ord) {
        const uint32_t idx = constraint_eval_order == nullptr ? ord : constraint_eval_order[ord];
        if (idx >= constraint_count) {
            return SeedStatus::reject;
        }
        const NativeQueryConstraintDesc &rc = constraints[idx];
        ConstraintEvalStats *step_stats = nullptr;
        if constexpr (TrackStats) {
            if (constraint_stats != nullptr && idx < constraint_stats_count) {
                step_stats = &constraint_stats[idx];
                ++step_stats->evaluation_count;
            }
        }

        const uint32_t min_required = std::max<uint32_t>(1U, rc.min_required);
        uint32_t target_cap =
            target_caps == nullptr ? std::max<uint32_t>(min_required, rc.candidate_cap) : target_caps[idx];
        target_cap = std::max<uint32_t>(1U, std::min<uint32_t>(target_cap, rc.candidate_cap));
        const uint32_t stop_threshold = rc.quad_max_span == 0U ? min_required : target_cap;
        const bool store_matches =
            idx < plan.constraint_store_mask.size() && plan.constraint_store_mask[idx] != 0U;
        const bool keep_match_points = store_matches || rc.quad_max_span > 0U;

        std::vector<uint64_t> &constraint_matches = scratch.matches_by_constraint[idx];
        constraint_matches.clear();
        scratch.constraint_seen_dedupe.prepare(stop_threshold);
        uint32_t unique_match_count = 0U;
        bool generator_failed = false;

        auto mark_reject = [&](SeedStatus status) {
            if constexpr (TrackStats) {
                if (step_stats != nullptr) {
                    ++step_stats->reject_count;
                }
            }
            if (per_constraint_rejects != nullptr && idx < constraint_count) {
                ++per_constraint_rejects[idx];
            }
            return status;
        };

        auto process_anchor = [&](int32_t ax, int32_t az) {
            if (unique_match_count >= stop_threshold) {
                return true;
            }
            const uint32_t remaining = stop_threshold - unique_match_count;
            if (remaining == 0U) {
                return true;
            }
            const uint32_t collect_cap = std::min<uint32_t>(target_cap, remaining);
            const bool count_cap_prune = collect_cap >= rc.candidate_cap;

            void *collect_world = g_world;
            if (constraint_requires_exact_generator(rc)) {
                collect_world = ensure_world(rc.dimension);
                if (collect_world == nullptr) {
                    generator_failed = true;
                    return true;
                }
            }
            scratch.dedupe.prepare(collect_cap);
            cap_pruned_out += collect_constraint_candidates_for_mode_emit(
                api,
                scratch,
                seed,
                mc_version,
                regions,
                region_count,
                rc,
                ax,
                az,
                collect_cap,
                count_cap_prune,
                collect_world,
                false,
                [&](uint64_t packed) {
                    if (!scratch.constraint_seen_dedupe.insert(packed)) {
                        return;
                    }
                    ++unique_match_count;
                    if (keep_match_points) {
                        constraint_matches.push_back(packed);
                    }
                }
            );
            return unique_match_count >= stop_threshold;
        };

        if (rc.anchor_type == SCAN_ANCHOR_ORIGIN) {
            process_anchor(0, 0);
        } else if (rc.anchor_type == SCAN_ANCHOR_SPAWN) {
            int32_t sx = 0;
            int32_t sz = 0;
            if (!query_get_spawn(api, st, mc_version, seed, scratch, g_world, sx, sz)) {
                return mark_reject(SeedStatus::reject);
            }
            process_anchor(sx, sz);
        } else if (rc.anchor_type == SCAN_ANCHOR_FIXED) {
            process_anchor(rc.anchor_x, rc.anchor_z);
        } else if (rc.anchor_type == SCAN_ANCHOR_DEP) {
            if (rc.anchor_dep_index < 0 || static_cast<uint32_t>(rc.anchor_dep_index) >= constraint_count) {
                return mark_reject(SeedStatus::reject);
            }
            const std::vector<uint64_t> &deps =
                scratch.matches_by_constraint[static_cast<uint32_t>(rc.anchor_dep_index)];
            if (deps.empty()) {
                return mark_reject(SeedStatus::reject);
            }
            for (uint64_t packed : deps) {
                if (process_anchor(unpack_x_from_packed(packed), unpack_z_from_packed(packed))) {
                    break;
                }
            }
        } else {
            return mark_reject(SeedStatus::reject);
        }

        if (generator_failed) {
            return mark_reject(SeedStatus::reject);
        }
        if (unique_match_count < min_required) {
            return mark_reject(SeedStatus::reject);
        }
        if (rc.quad_max_span > 0U &&
            !has_compact_group_within_span(constraint_matches, min_required, rc.quad_max_span)) {
            return mark_reject(SeedStatus::reject);
        }
        if constexpr (TrackStats) {
            if (step_stats != nullptr) {
                ++step_stats->accept_count;
            }
        }
    }
    return SeedStatus::valid;
}

static bool evaluate_compiled_biome_group_at_anchor(
    const CubiApi &api,
    QueryScratch &scratch,
    void *g_world,
    int32_t dimension,
    const CompiledBiomeGroup &group,
    const std::vector<QueryBiomePrepared> &prepared_filters,
    int32_t anchor_x,
    int32_t anchor_z,
    std::vector<uint8_t> &filter_satisfied,
    std::vector<NativeQueryBiomeOut> *records
) {
    if (g_world == nullptr) {
        return false;
    }
    scratch.grouped_biome_ids.resize(group.sample_offsets.size());
    for (uint32_t i = 0; i < group.sample_offsets.size(); ++i) {
        const BiomeOffset &offset = group.sample_offsets[i];
        scratch.grouped_biome_ids[i] = cached_query_biome_at(
            api,
            scratch,
            g_world,
            dimension,
            anchor_x + offset.dx,
            group.y,
            anchor_z + offset.dz
        );
    }

    for (const CompiledBiomeGroupFilter &filter_ref : group.filters) {
        if (filter_ref.filter_index >= prepared_filters.size()) {
            continue;
        }
        if (filter_ref.filter_index < filter_satisfied.size() && filter_satisfied[filter_ref.filter_index] != 0U) {
            continue;
        }
        const QueryBiomePrepared &prepared = prepared_filters[filter_ref.filter_index];
        if (prepared.offsets == nullptr) {
            continue;
        }
        for (uint32_t ord = 0; ord < filter_ref.sample_indices.size() && ord < prepared.offsets->size(); ++ord) {
            const uint32_t sample_idx = filter_ref.sample_indices[ord];
            if (sample_idx >= scratch.grouped_biome_ids.size()) {
                continue;
            }
            const int32_t biome_id = scratch.grouped_biome_ids[sample_idx];
            if (!contains_biome_id(prepared.allowed_sorted, biome_id)) {
                continue;
            }
            if (filter_ref.filter_index < filter_satisfied.size()) {
                filter_satisfied[filter_ref.filter_index] = 1U;
            }
            if (records != nullptr && filter_ref.filter_index < records->size()) {
                const BiomeOffset &offset = (*prepared.offsets)[ord];
                NativeQueryBiomeOut &record = (*records)[filter_ref.filter_index];
                record.filter_index = filter_ref.filter_index;
                record.x = anchor_x + offset.dx;
                record.y = group.y;
                record.z = anchor_z + offset.dz;
                record.radius = prepared.desc.radius;
                record.dist2 = offset.d2;
                record.biome_id = biome_id;
            }
            break;
        }
    }
    return true;
}

static SeedStatus evaluate_query_seed_early_fixed_biomes_compiled(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const CompiledQueryPlan &plan
) {
    if (!plan.has_early_fixed_biome_filters) {
        return SeedStatus::valid;
    }
    if (!api.direct_ready) {
        return SeedStatus::biome_reject;
    }

    void *g_world = nullptr;
    for (const QueryBiomePrepared &bf : plan.prepared_filters) {
        if (!biome_filter_is_cheap_exact_early_anchor(bf)) {
            continue;
        }
        int32_t point_x = 0;
        int32_t point_z = 0;
        if (!resolve_exact_early_biome_anchor_point(
                api,
                st,
                scratch,
                seed,
                mc_version,
                g_world,
                bf.desc,
                point_x,
                point_z)) {
            return SeedStatus::biome_reject;
        }
        if (!query_biome_filter_matches_at_point(
                api,
                st,
                scratch,
                seed,
                mc_version,
                g_world,
                bf,
                point_x,
                point_z)) {
            return SeedStatus::biome_reject;
        }
    }
    return SeedStatus::valid;
}

static SeedStatus evaluate_query_seed_stage_c_biomes_compiled(
    const CubiApi &api,
    ThreadCubiState &st,
    QueryScratch &scratch,
    uint64_t seed,
    int mc_version,
    const CompiledQueryPlan &plan,
    std::vector<NativeQueryBiomeOut> *records
) {
    if (plan.prepared_filters.empty()) {
        if (records != nullptr) {
            records->clear();
        }
        return SeedStatus::valid;
    }
    if (!api.direct_ready) {
        return SeedStatus::biome_reject;
    }

    if (records != nullptr) {
        records->clear();
        records->resize(plan.prepared_filters.size());
    }
    std::vector<uint8_t> filter_satisfied(plan.prepared_filters.size(), 0U);
    if (records == nullptr && plan.has_early_fixed_biome_filters) {
        for (uint32_t i = 0; i < plan.prepared_filters.size(); ++i) {
            if (biome_filter_is_cheap_exact_early_anchor(plan.prepared_filters[i])) {
                filter_satisfied[i] = 1U;
            }
        }
    }
    void *g_world = nullptr;
    auto ensure_world = [&](int32_t dimension) -> void * {
        g_world = ensure_generator(api, st, mc_version, seed, dimension);
        return g_world;
    };

    for (const CompiledBiomeGroup &group : plan.stage_c_biome_groups) {
        bool group_already_satisfied = true;
        for (const CompiledBiomeGroupFilter &filter_ref : group.filters) {
            if (filter_ref.filter_index < filter_satisfied.size() &&
                filter_satisfied[filter_ref.filter_index] != 0U) {
                continue;
            }
            group_already_satisfied = false;
            break;
        }
        if (group_already_satisfied) {
            continue;
        }

        auto process_anchor = [&](int32_t ax, int32_t az) {
            return evaluate_compiled_biome_group_at_anchor(
                api,
                scratch,
                ensure_world(group.dimension),
                group.dimension,
                group,
                plan.prepared_filters,
                ax,
                az,
                filter_satisfied,
                records
            );
        };

        bool anchor_ok = false;
        if (group.point_type == SCAN_ANCHOR_ORIGIN) {
            anchor_ok = process_anchor(0, 0);
        } else if (group.point_type == SCAN_ANCHOR_FIXED) {
            anchor_ok = process_anchor(group.point_x, group.point_z);
        } else if (group.point_type == SCAN_ANCHOR_SPAWN) {
            int32_t sx = 0;
            int32_t sz = 0;
            if (!query_get_spawn(api, st, mc_version, seed, scratch, g_world, sx, sz)) {
                return SeedStatus::biome_reject;
            }
            anchor_ok = process_anchor(sx, sz);
        } else if (group.point_type == SCAN_ANCHOR_DEP) {
            if (group.point_dep_index < 0 ||
                static_cast<uint32_t>(group.point_dep_index) >= plan.constraints.size()) {
                return SeedStatus::biome_reject;
            }
            const std::vector<uint64_t> &deps =
                scratch.matches_by_constraint[static_cast<uint32_t>(group.point_dep_index)];
            for (uint64_t packed : deps) {
                std::vector<uint8_t> candidate_satisfied = filter_satisfied;
                if (!evaluate_compiled_biome_group_at_anchor(
                        api,
                        scratch,
                        ensure_world(group.dimension),
                        group.dimension,
                        group,
                        plan.prepared_filters,
                        unpack_x_from_packed(packed),
                        unpack_z_from_packed(packed),
                        candidate_satisfied,
                        records)) {
                    continue;
                }
                bool group_done = true;
                for (const CompiledBiomeGroupFilter &filter_ref : group.filters) {
                    if (filter_ref.filter_index >= candidate_satisfied.size() ||
                        candidate_satisfied[filter_ref.filter_index] != 0U) {
                        continue;
                    }
                    group_done = false;
                    break;
                }
                if (group_done) {
                    filter_satisfied = std::move(candidate_satisfied);
                    anchor_ok = true;
                    break;
                }
            }
        }
        if (!anchor_ok) {
            return SeedStatus::biome_reject;
        }
        for (const CompiledBiomeGroupFilter &filter_ref : group.filters) {
            if (filter_ref.filter_index >= filter_satisfied.size() ||
                filter_satisfied[filter_ref.filter_index] == 0U) {
                return SeedStatus::biome_reject;
            }
        }
    }

    if (records != nullptr) {
        std::vector<NativeQueryBiomeOut> compact;
        compact.reserve(plan.prepared_filters.size());
        for (uint32_t i = 0; i < records->size(); ++i) {
            if (i < filter_satisfied.size() && filter_satisfied[i] != 0U) {
                compact.push_back((*records)[i]);
            }
        }
        *records = std::move(compact);
    }
    return SeedStatus::valid;
}

template <typename WorkerFn>
static int run_parallel_batch(
    uint32_t seed_count,
    uint32_t worker_count,
    WorkerFn &&worker,
    std::vector<BatchTaskResult> &task_results
) {
    if (seed_count == 0U) {
        task_results.clear();
        return 0;
    }
    const uint32_t requested_workers = std::max<uint32_t>(1U, std::min<uint32_t>(worker_count, seed_count));
    constexpr uint32_t kTargetSeedsPerWorker = 512U;
    uint32_t workers_by_work = (seed_count + (kTargetSeedsPerWorker - 1U)) / kTargetSeedsPerWorker;
    if (workers_by_work == 0U) {
        workers_by_work = 1U;
    }
    const uint32_t workers = std::max<uint32_t>(1U, std::min<uint32_t>(requested_workers, workers_by_work));
    task_results.assign(workers, BatchTaskResult{});

    if (workers == 1U) {
        worker(0U, seed_count, task_results[0]);
        return 0;
    }

    std::vector<std::thread> threads;
    threads.reserve(workers);

    uint32_t start = 0;
    const uint32_t base = seed_count / workers;
    const uint32_t rem = seed_count % workers;

    for (uint32_t w = 0; w < workers; ++w) {
        const uint32_t span = base + (w < rem ? 1U : 0U);
        const uint32_t end = start + span;
        BatchTaskResult *task_ptr = &task_results[w];
        threads.emplace_back([&worker, task_ptr, start, end]() { worker(start, end, *task_ptr); });
        start = end;
    }

    for (std::thread &t : threads) {
        t.join();
    }
    return 0;
}

static std::unique_ptr<CompiledQueryPlan> compile_query_plan_internal(
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count,
    int &rc_out
) {
    rc_out = validate_query_descriptors(regions, region_count, constraints, constraint_count, biome_filters, biome_filter_count);
    if (rc_out != 0) {
        return nullptr;
    }

    std::unique_ptr<CompiledQueryPlan> plan(new CompiledQueryPlan{});
    if (regions != nullptr && region_count > 0U) {
        plan->regions.assign(regions, regions + region_count);
    }
    if (constraints != nullptr && constraint_count > 0U) {
        plan->constraints.assign(constraints, constraints + constraint_count);
    }
    if (biome_filters != nullptr && biome_filter_count > 0U) {
        plan->biome_filters.assign(biome_filters, biome_filters + biome_filter_count);
    }
    if (biome_allowed_ids != nullptr && biome_allowed_count > 0U) {
        plan->biome_allowed_ids.assign(biome_allowed_ids, biome_allowed_ids + biome_allowed_count);
    }

    rc_out = prepare_query_biome_filters(
        plan->biome_filters.empty() ? nullptr : plan->biome_filters.data(),
        static_cast<uint32_t>(plan->biome_filters.size()),
        plan->biome_allowed_ids.empty() ? nullptr : plan->biome_allowed_ids.data(),
        static_cast<uint32_t>(plan->biome_allowed_ids.size()),
        plan->prepared_filters
    );
    if (rc_out != 0) {
        return nullptr;
    }

    plan->direct_api_needed = !plan->prepared_filters.empty();
    plan->generator_needed = !plan->prepared_filters.empty();
    for (const NativeQueryConstraintDesc &constraint : plan->constraints) {
        if (constraint_needs_direct_api(constraint)) {
            plan->direct_api_needed = true;
        }
        if (constraint_needs_generator(constraint)) {
            plan->generator_needed = true;
        }
    }

    const NativeBiomeFilterDesc *biome_filters_ptr = plan->biome_filters.empty() ? nullptr : plan->biome_filters.data();
    const NativeQueryConstraintDesc *constraints_ptr = plan->constraints.empty() ? nullptr : plan->constraints.data();
    const uint32_t constraint_count_u32 = static_cast<uint32_t>(plan->constraints.size());
    const uint32_t biome_filter_count_u32 = static_cast<uint32_t>(plan->biome_filters.size());

    plan->fast_target_caps = build_fast_target_caps(
        constraints_ptr,
        constraint_count_u32,
        biome_filters_ptr,
        biome_filter_count_u32
    );
    const uint32_t *fast_target_caps_ptr = plan->fast_target_caps.empty() ? nullptr : plan->fast_target_caps.data();
    plan->constraint_store_mask = build_constraint_dependency_mask(
        constraints_ptr,
        constraint_count_u32,
        biome_filters_ptr,
        biome_filter_count_u32
    );
    plan->effective_target_caps =
        build_effective_target_caps(constraints_ptr, constraint_count_u32, fast_target_caps_ptr);
    plan->base_constraint_eval_order = build_constraint_eval_order(constraints_ptr, constraint_count_u32);
    const uint32_t *base_order_ptr =
        plan->base_constraint_eval_order.empty() ? nullptr : plan->base_constraint_eval_order.data();
    plan->base_stage0_constraint_eval_order =
        build_stage0_constraint_eval_order(constraints_ptr, constraint_count_u32, base_order_ptr);
    if (constraint_count_u32 > 0U &&
        (base_order_ptr == nullptr || plan->effective_target_caps.size() != constraint_count_u32)) {
        rc_out = -1;
        return nullptr;
    }

    plan->stage_c_biome_groups = build_compiled_biome_groups(plan->prepared_filters);
    plan->has_early_fixed_biome_filters = false;
    for (const QueryBiomePrepared &bf : plan->prepared_filters) {
        if (biome_filter_is_cheap_exact_early_anchor(bf)) {
            plan->has_early_fixed_biome_filters = true;
            break;
        }
    }
    build_compiled_stage_partitions(*plan);
    rc_out = 0;
    return plan;
}

static void store_compiled_batch_stats(
    const CompiledQueryPlan &plan,
    const CompiledBatchStatsAccumulator &stats
) {
    std::lock_guard<std::mutex> lock(plan.batch_stats_mu);
    plan.last_per_constraint_stage_a_rejects = stats.per_constraint_stage_a_rejects;
    plan.last_per_constraint_stage_a5_rejects = stats.per_constraint_stage_a5_rejects;
    plan.last_per_constraint_stage_b_exact_rejects = stats.per_constraint_stage_b_exact_rejects;

    ScannerCompiledBatchStats public_stats{};
    public_stats.input_seed_count = stats.input_seed_count;
    public_stats.stage_a_survivor_count = stats.stage_a_survivor_count;
    public_stats.stage_a5_survivor_count = stats.stage_a5_survivor_count;
    public_stats.stage_b_structure_survivor_count = stats.stage_b_structure_survivor_count;
    public_stats.stage_c_biome_survivor_count = stats.stage_c_biome_survivor_count;
    public_stats.final_valid_count = stats.final_valid_count;
    public_stats.stage_a_reject_count = stats.stage_a_reject_count;
    public_stats.stage_a5_reject_count = stats.stage_a5_reject_count;
    public_stats.stage_b_exact_reject_count = stats.stage_b_exact_reject_count;
    public_stats.stage_c_biome_reject_count = stats.stage_c_biome_reject_count;
    public_stats.setup_generator_calls = stats.cubi_calls.setup_generator_calls;
    public_stats.apply_seed_calls = stats.cubi_calls.apply_seed_calls;
    public_stats.get_structure_pos_calls = stats.cubi_calls.get_structure_pos_calls;
    public_stats.is_viable_structure_pos_calls = stats.cubi_calls.is_viable_structure_pos_calls;
    public_stats.get_biome_at_calls = stats.cubi_calls.get_biome_at_calls;
    public_stats.get_spawn_calls = stats.cubi_calls.get_spawn_calls;
    public_stats.estimate_spawn_calls = stats.cubi_calls.estimate_spawn_calls;
    public_stats.init_first_stronghold_calls = stats.cubi_calls.init_first_stronghold_calls;
    public_stats.next_stronghold_calls = stats.cubi_calls.next_stronghold_calls;
    public_stats.cpu_stage_a5_seconds = stats.cpu_stage_a5_seconds;
    public_stats.cpu_stage_b_seconds = stats.cpu_stage_b_seconds;
    public_stats.cpu_stage_c_seconds = stats.cpu_stage_c_seconds;
    public_stats.constraint_count = static_cast<uint32_t>(plan.constraints.size());
    public_stats.per_constraint_stage_a_rejects =
        plan.last_per_constraint_stage_a_rejects.empty() ? nullptr : plan.last_per_constraint_stage_a_rejects.data();
    public_stats.per_constraint_stage_a5_rejects =
        plan.last_per_constraint_stage_a5_rejects.empty() ? nullptr : plan.last_per_constraint_stage_a5_rejects.data();
    public_stats.per_constraint_stage_b_exact_rejects = plan.last_per_constraint_stage_b_exact_rejects.empty()
                                                            ? nullptr
                                                            : plan.last_per_constraint_stage_b_exact_rejects.data();
    plan.last_batch_stats = public_stats;
}

static void merge_compiled_batch_stats(
    CompiledBatchStatsAccumulator &dst,
    const CompiledBatchStatsAccumulator &src
) {
    dst.stage_a_survivor_count += src.stage_a_survivor_count;
    dst.stage_a5_survivor_count += src.stage_a5_survivor_count;
    dst.stage_b_structure_survivor_count += src.stage_b_structure_survivor_count;
    dst.stage_c_biome_survivor_count += src.stage_c_biome_survivor_count;
    dst.final_valid_count += src.final_valid_count;
    dst.stage_a_reject_count += src.stage_a_reject_count;
    dst.stage_a5_reject_count += src.stage_a5_reject_count;
    dst.stage_b_exact_reject_count += src.stage_b_exact_reject_count;
    dst.stage_c_biome_reject_count += src.stage_c_biome_reject_count;
    dst.cpu_stage_a5_seconds += src.cpu_stage_a5_seconds;
    dst.cpu_stage_b_seconds += src.cpu_stage_b_seconds;
    dst.cpu_stage_c_seconds += src.cpu_stage_c_seconds;
    dst.cubi_calls.setup_generator_calls += src.cubi_calls.setup_generator_calls;
    dst.cubi_calls.apply_seed_calls += src.cubi_calls.apply_seed_calls;
    dst.cubi_calls.get_structure_pos_calls += src.cubi_calls.get_structure_pos_calls;
    dst.cubi_calls.is_viable_structure_pos_calls += src.cubi_calls.is_viable_structure_pos_calls;
    dst.cubi_calls.get_biome_at_calls += src.cubi_calls.get_biome_at_calls;
    dst.cubi_calls.get_spawn_calls += src.cubi_calls.get_spawn_calls;
    dst.cubi_calls.estimate_spawn_calls += src.cubi_calls.estimate_spawn_calls;
    dst.cubi_calls.init_first_stronghold_calls += src.cubi_calls.init_first_stronghold_calls;
    dst.cubi_calls.next_stronghold_calls += src.cubi_calls.next_stronghold_calls;

    const size_t stage_a_count =
        std::min(dst.per_constraint_stage_a_rejects.size(), src.per_constraint_stage_a_rejects.size());
    for (size_t i = 0; i < stage_a_count; ++i) {
        dst.per_constraint_stage_a_rejects[i] += src.per_constraint_stage_a_rejects[i];
    }
    const size_t stage_a5_count =
        std::min(dst.per_constraint_stage_a5_rejects.size(), src.per_constraint_stage_a5_rejects.size());
    for (size_t i = 0; i < stage_a5_count; ++i) {
        dst.per_constraint_stage_a5_rejects[i] += src.per_constraint_stage_a5_rejects[i];
    }
    const size_t stage_b_count =
        std::min(dst.per_constraint_stage_b_exact_rejects.size(), src.per_constraint_stage_b_exact_rejects.size());
    for (size_t i = 0; i < stage_b_count; ++i) {
        dst.per_constraint_stage_b_exact_rejects[i] += src.per_constraint_stage_b_exact_rejects[i];
    }
}

static int validate_query_batch_with_plan(
    const CompiledQueryPlan &plan,
    const uint64_t *seeds,
    uint32_t seed_count,
    int32_t cubi_mc_version,
    uint32_t worker_count,
    uint64_t *valid_out,
    uint32_t *valid_count,
    uint32_t *mismatch_count,
    uint32_t *biome_reject_count,
    uint32_t *cap_pruned_total,
    bool skip_stage_a = false
) {
    if ((seed_count > 0U && seeds == nullptr) || valid_out == nullptr || valid_count == nullptr ||
        mismatch_count == nullptr || biome_reject_count == nullptr || cap_pruned_total == nullptr) {
        return -1;
    }

    CubiApi api_copy{};
    {
        std::lock_guard<std::mutex> lock(g_cubi_mu);
        api_copy = g_cubi;
    }
    if (plan.direct_api_needed) {
        if (!api_copy.loaded) {
            return -2;
        }
        if (!api_copy.direct_ready) {
            return -3;
        }
    }

    constexpr uint32_t kSelectivityWarmupSeeds = 4096U;
    constexpr uint64_t kSelectivityMinEvalSamples = 32ULL;
    constexpr uint32_t kDynamicChunkSize = 512U;
    constexpr bool kCollectDetailedStageTiming = false;
    const uint32_t constraint_count = static_cast<uint32_t>(plan.constraints.size());
    CompiledBatchStatsAccumulator batch_stats(constraint_count);
    batch_stats.input_seed_count = seed_count;
    batch_stats.stage_a_survivor_count = seed_count;
    auto run_early_fixed_biomes = [&](
                                      ThreadCubiState &cubi_state,
                                      QueryScratch &scratch,
                                      uint64_t seed,
                                      auto &stats
                                  ) -> SeedStatus {
        if (!plan.has_early_fixed_biome_filters) {
            return SeedStatus::valid;
        }
        std::chrono::high_resolution_clock::time_point stage_c_t0{};
        if (kCollectDetailedStageTiming) {
            stage_c_t0 = std::chrono::high_resolution_clock::now();
        }
        const SeedStatus status = evaluate_query_seed_early_fixed_biomes_compiled(
            api_copy,
            cubi_state,
            scratch,
            seed,
            cubi_mc_version,
            plan
        );
        if (kCollectDetailedStageTiming) {
            stats.cpu_stage_c_seconds +=
                std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_c_t0).count();
        }
        return status;
    };
    const uint32_t warmup_seed_count =
        constraint_count > 1U ? std::min<uint32_t>(seed_count, kSelectivityWarmupSeeds) : 0U;

    std::vector<ConstraintEvalStats> warmup_stats(constraint_count);
    BatchTaskResult warmup_result;
    warmup_result.valid.reserve(static_cast<size_t>(warmup_seed_count));

    if (warmup_seed_count > 0U) {
        ThreadCubiState warmup_cubi_state{};
        QueryScratch warmup_scratch(constraint_count, plan.constraints.empty() ? nullptr : plan.constraints.data());
        warmup_cubi_state.call_counters = &batch_stats.cubi_calls;
        warmup_scratch.call_counters = &batch_stats.cubi_calls;
        ConstraintEvalStats *warmup_stats_ptr = warmup_stats.empty() ? nullptr : warmup_stats.data();
        const uint32_t *base_order_ptr =
            plan.base_constraint_eval_order.empty() ? nullptr : plan.base_constraint_eval_order.data();
        const uint32_t *effective_caps_ptr =
            plan.effective_target_caps.empty() ? nullptr : plan.effective_target_caps.data();
        for (uint32_t i = 0; i < warmup_seed_count; ++i) {
            if (!skip_stage_a) {
                uint32_t stage_a_cap_pruned = 0U;
                std::chrono::high_resolution_clock::time_point stage_a_t0{};
                if (kCollectDetailedStageTiming) {
                    stage_a_t0 = std::chrono::high_resolution_clock::now();
                }
                const SeedStatus stage_a_status = evaluate_query_seed_stage_a_compiled(
                    warmup_scratch,
                    seeds[i],
                    plan,
                    stage_a_cap_pruned,
                    batch_stats.per_constraint_stage_a5_rejects.data()
                );
                if (kCollectDetailedStageTiming) {
                    batch_stats.cpu_stage_a5_seconds +=
                        std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_a_t0).count();
                }
                warmup_result.cap_pruned += stage_a_cap_pruned;
                if (stage_a_status != SeedStatus::valid) {
                    ++batch_stats.stage_a5_reject_count;
                    ++warmup_result.mismatch;
                    continue;
                }
            }
            ++batch_stats.stage_a5_survivor_count;

            const SeedStatus early_biome_status =
                run_early_fixed_biomes(warmup_cubi_state, warmup_scratch, seeds[i], batch_stats);
            if (early_biome_status != SeedStatus::valid) {
                ++batch_stats.stage_c_biome_reject_count;
                ++warmup_result.biome_reject;
                continue;
            }

            uint32_t stage_b_cap_pruned = 0U;
            std::chrono::high_resolution_clock::time_point stage_b_t0{};
            if (kCollectDetailedStageTiming) {
                stage_b_t0 = std::chrono::high_resolution_clock::now();
            }
            SeedStatus status = evaluate_query_seed_stage_b_constraints_compiled<true>(
                api_copy,
                warmup_cubi_state,
                warmup_scratch,
                seeds[i],
                cubi_mc_version,
                plan,
                base_order_ptr,
                effective_caps_ptr,
                stage_b_cap_pruned,
                warmup_stats_ptr,
                static_cast<uint32_t>(warmup_stats.size()),
                batch_stats.per_constraint_stage_b_exact_rejects.data()
            );
            if (kCollectDetailedStageTiming) {
                batch_stats.cpu_stage_b_seconds +=
                    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_b_t0).count();
            }
            warmup_result.cap_pruned += stage_b_cap_pruned;
            if (status != SeedStatus::valid) {
                ++batch_stats.stage_b_exact_reject_count;
            } else {
                ++batch_stats.stage_b_structure_survivor_count;
            }
            if (status == SeedStatus::valid) {
                std::chrono::high_resolution_clock::time_point stage_c_t0{};
                if (kCollectDetailedStageTiming) {
                    stage_c_t0 = std::chrono::high_resolution_clock::now();
                }
                status = evaluate_query_seed_stage_c_biomes_compiled(
                    api_copy,
                    warmup_cubi_state,
                    warmup_scratch,
                    seeds[i],
                    cubi_mc_version,
                    plan,
                    nullptr
                );
                if (kCollectDetailedStageTiming) {
                    batch_stats.cpu_stage_c_seconds +=
                        std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_c_t0).count();
                }
            }
            if (status == SeedStatus::valid) {
                ++batch_stats.stage_c_biome_survivor_count;
                ++batch_stats.final_valid_count;
                warmup_result.valid.push_back(seeds[i]);
            } else if (status == SeedStatus::biome_reject) {
                ++batch_stats.stage_c_biome_reject_count;
                ++warmup_result.biome_reject;
            } else {
                ++warmup_result.mismatch;
            }
        }
    }

    std::vector<uint32_t> runtime_constraint_eval_order = plan.base_constraint_eval_order;
    if (warmup_seed_count > 0U && constraint_count > 1U) {
        const std::vector<uint32_t> selective_order = build_constraint_eval_order_by_selectivity(
            plan.constraints.empty() ? nullptr : plan.constraints.data(),
            constraint_count,
            plan.base_constraint_eval_order,
            warmup_stats,
            kSelectivityMinEvalSamples
        );
        if (selective_order.size() == constraint_count) {
            runtime_constraint_eval_order = selective_order;
        }
    }
    const uint32_t *runtime_order_ptr =
        runtime_constraint_eval_order.empty() ? nullptr : runtime_constraint_eval_order.data();
    const uint32_t *effective_caps_ptr =
        plan.effective_target_caps.empty() ? nullptr : plan.effective_target_caps.data();
    if (constraint_count > 0U && runtime_order_ptr == nullptr) {
        return -1;
    }

    const uint32_t remaining_seed_count = seed_count - warmup_seed_count;
    std::vector<BatchChunkResult> chunk_results;
    if (remaining_seed_count > 0U) {
        const uint32_t requested_workers = std::max<uint32_t>(1U, std::min<uint32_t>(worker_count, remaining_seed_count));
        const uint32_t chunk_count = (remaining_seed_count + (kDynamicChunkSize - 1U)) / kDynamicChunkSize;
        const uint32_t workers = std::max<uint32_t>(1U, std::min<uint32_t>(requested_workers, chunk_count));
        chunk_results.assign(chunk_count, BatchChunkResult{});

        if (workers == 1U) {
            ThreadCubiState cubi_state{};
            QueryScratch scratch(constraint_count, plan.constraints.empty() ? nullptr : plan.constraints.data());
            CompiledBatchStatsAccumulator local_stats(constraint_count);
            cubi_state.call_counters = &local_stats.cubi_calls;
            scratch.call_counters = &local_stats.cubi_calls;
            for (uint32_t chunk_idx = 0; chunk_idx < chunk_count; ++chunk_idx) {
                const uint32_t start = chunk_idx * kDynamicChunkSize;
                const uint32_t end = std::min<uint32_t>(remaining_seed_count, start + kDynamicChunkSize);
                BatchChunkResult &out = chunk_results[chunk_idx];
                out.valid.reserve(static_cast<size_t>(end - start));
                for (uint32_t i = start; i < end; ++i) {
                    const uint32_t seed_idx = warmup_seed_count + i;
                    if (!skip_stage_a) {
                        uint32_t stage_a_cap_pruned = 0U;
                        std::chrono::high_resolution_clock::time_point stage_a_t0{};
                        if (kCollectDetailedStageTiming) {
                            stage_a_t0 = std::chrono::high_resolution_clock::now();
                        }
                        const SeedStatus stage_a_status =
                            evaluate_query_seed_stage_a_compiled(
                                scratch,
                                seeds[seed_idx],
                                plan,
                                stage_a_cap_pruned,
                                local_stats.per_constraint_stage_a5_rejects.data());
                        if (kCollectDetailedStageTiming) {
                            local_stats.cpu_stage_a5_seconds +=
                                std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_a_t0).count();
                        }
                        out.cap_pruned += stage_a_cap_pruned;
                        if (stage_a_status != SeedStatus::valid) {
                            ++local_stats.stage_a5_reject_count;
                            ++out.mismatch;
                            continue;
                        }
                    }
                    ++local_stats.stage_a5_survivor_count;
                    const SeedStatus early_biome_status =
                        run_early_fixed_biomes(cubi_state, scratch, seeds[seed_idx], local_stats);
                    if (early_biome_status != SeedStatus::valid) {
                        ++local_stats.stage_c_biome_reject_count;
                        ++out.biome_reject;
                        continue;
                    }
                    uint32_t stage_b_cap_pruned = 0U;
                    std::chrono::high_resolution_clock::time_point stage_b_t0{};
                    if (kCollectDetailedStageTiming) {
                        stage_b_t0 = std::chrono::high_resolution_clock::now();
                    }
                    SeedStatus status = evaluate_query_seed_stage_b_constraints_compiled<false>(
                        api_copy,
                        cubi_state,
                        scratch,
                        seeds[seed_idx],
                        cubi_mc_version,
                        plan,
                        runtime_order_ptr,
                        effective_caps_ptr,
                        stage_b_cap_pruned,
                        nullptr,
                        0U,
                        local_stats.per_constraint_stage_b_exact_rejects.data()
                    );
                    if (kCollectDetailedStageTiming) {
                        local_stats.cpu_stage_b_seconds +=
                            std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_b_t0).count();
                    }
                    out.cap_pruned += stage_b_cap_pruned;
                    if (status != SeedStatus::valid) {
                        ++local_stats.stage_b_exact_reject_count;
                    } else {
                        ++local_stats.stage_b_structure_survivor_count;
                    }
                    if (status == SeedStatus::valid) {
                        std::chrono::high_resolution_clock::time_point stage_c_t0{};
                        if (kCollectDetailedStageTiming) {
                            stage_c_t0 = std::chrono::high_resolution_clock::now();
                        }
                        status = evaluate_query_seed_stage_c_biomes_compiled(
                            api_copy,
                            cubi_state,
                            scratch,
                            seeds[seed_idx],
                            cubi_mc_version,
                            plan,
                            nullptr
                        );
                        if (kCollectDetailedStageTiming) {
                            local_stats.cpu_stage_c_seconds +=
                                std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_c_t0)
                                    .count();
                        }
                    }
                    if (status == SeedStatus::valid) {
                        ++local_stats.stage_c_biome_survivor_count;
                        ++local_stats.final_valid_count;
                        out.valid.push_back(seeds[seed_idx]);
                    } else if (status == SeedStatus::biome_reject) {
                        ++local_stats.stage_c_biome_reject_count;
                        ++out.biome_reject;
                    } else {
                        ++out.mismatch;
                    }
                }
            }
            merge_compiled_batch_stats(batch_stats, local_stats);
        } else {
            std::atomic<uint32_t> next_chunk{0U};
            std::vector<std::thread> threads;
            threads.reserve(workers);
            std::vector<CompiledBatchStatsAccumulator> worker_stats;
            worker_stats.reserve(workers);
            for (uint32_t w = 0; w < workers; ++w) {
                worker_stats.emplace_back(constraint_count);
            }
            for (uint32_t w = 0; w < workers; ++w) {
                threads.emplace_back([&, w]() {
                    ThreadCubiState cubi_state{};
                    QueryScratch scratch(constraint_count, plan.constraints.empty() ? nullptr : plan.constraints.data());
                    CompiledBatchStatsAccumulator &local_stats = worker_stats[w];
                    cubi_state.call_counters = &local_stats.cubi_calls;
                    scratch.call_counters = &local_stats.cubi_calls;
                    while (true) {
                        const uint32_t chunk_idx = next_chunk.fetch_add(1U, std::memory_order_relaxed);
                        if (chunk_idx >= chunk_count) {
                            break;
                        }
                        const uint32_t start = chunk_idx * kDynamicChunkSize;
                        const uint32_t end = std::min<uint32_t>(remaining_seed_count, start + kDynamicChunkSize);
                        BatchChunkResult &out = chunk_results[chunk_idx];
                        out.valid.reserve(static_cast<size_t>(end - start));
                        for (uint32_t i = start; i < end; ++i) {
                            const uint32_t seed_idx = warmup_seed_count + i;
                            if (!skip_stage_a) {
                                uint32_t stage_a_cap_pruned = 0U;
                                std::chrono::high_resolution_clock::time_point stage_a_t0{};
                                if (kCollectDetailedStageTiming) {
                                    stage_a_t0 = std::chrono::high_resolution_clock::now();
                                }
                                const SeedStatus stage_a_status =
                                    evaluate_query_seed_stage_a_compiled(
                                        scratch,
                                        seeds[seed_idx],
                                        plan,
                                        stage_a_cap_pruned,
                                        local_stats.per_constraint_stage_a5_rejects.data());
                                if (kCollectDetailedStageTiming) {
                                    local_stats.cpu_stage_a5_seconds +=
                                        std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_a_t0)
                                            .count();
                                }
                                out.cap_pruned += stage_a_cap_pruned;
                                if (stage_a_status != SeedStatus::valid) {
                                    ++local_stats.stage_a5_reject_count;
                                    ++out.mismatch;
                                    continue;
                                }
                            }
                            ++local_stats.stage_a5_survivor_count;
                            const SeedStatus early_biome_status =
                                run_early_fixed_biomes(cubi_state, scratch, seeds[seed_idx], local_stats);
                            if (early_biome_status != SeedStatus::valid) {
                                ++local_stats.stage_c_biome_reject_count;
                                ++out.biome_reject;
                                continue;
                            }
                            uint32_t stage_b_cap_pruned = 0U;
                            std::chrono::high_resolution_clock::time_point stage_b_t0{};
                            if (kCollectDetailedStageTiming) {
                                stage_b_t0 = std::chrono::high_resolution_clock::now();
                            }
                            SeedStatus status = evaluate_query_seed_stage_b_constraints_compiled<false>(
                                api_copy,
                                cubi_state,
                                scratch,
                                seeds[seed_idx],
                                cubi_mc_version,
                                plan,
                                runtime_order_ptr,
                                effective_caps_ptr,
                                stage_b_cap_pruned,
                                nullptr,
                                0U,
                                local_stats.per_constraint_stage_b_exact_rejects.data()
                            );
                            if (kCollectDetailedStageTiming) {
                                local_stats.cpu_stage_b_seconds +=
                                    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - stage_b_t0)
                                        .count();
                            }
                            out.cap_pruned += stage_b_cap_pruned;
                            if (status != SeedStatus::valid) {
                                ++local_stats.stage_b_exact_reject_count;
                            } else {
                                ++local_stats.stage_b_structure_survivor_count;
                            }
                            if (status == SeedStatus::valid) {
                                std::chrono::high_resolution_clock::time_point stage_c_t0{};
                                if (kCollectDetailedStageTiming) {
                                    stage_c_t0 = std::chrono::high_resolution_clock::now();
                                }
                                status = evaluate_query_seed_stage_c_biomes_compiled(
                                    api_copy,
                                    cubi_state,
                                    scratch,
                                    seeds[seed_idx],
                                    cubi_mc_version,
                                    plan,
                                    nullptr
                                );
                                if (kCollectDetailedStageTiming) {
                                    local_stats.cpu_stage_c_seconds +=
                                        std::chrono::duration<double>(
                                            std::chrono::high_resolution_clock::now() - stage_c_t0)
                                            .count();
                                }
                            }
                            if (status == SeedStatus::valid) {
                                ++local_stats.stage_c_biome_survivor_count;
                                ++local_stats.final_valid_count;
                                out.valid.push_back(seeds[seed_idx]);
                            } else if (status == SeedStatus::biome_reject) {
                                ++local_stats.stage_c_biome_reject_count;
                                ++out.biome_reject;
                            } else {
                                ++out.mismatch;
                            }
                        }
                    }
                });
            }
            for (std::thread &thread : threads) {
                thread.join();
            }
            for (const CompiledBatchStatsAccumulator &local_stats : worker_stats) {
                merge_compiled_batch_stats(batch_stats, local_stats);
            }
        }
    }

    uint32_t valid_total = static_cast<uint32_t>(warmup_result.valid.size());
    uint32_t mismatch_total = warmup_result.mismatch;
    uint32_t biome_total = warmup_result.biome_reject;
    uint32_t cap_total = warmup_result.cap_pruned;
    for (const BatchChunkResult &chunk : chunk_results) {
        valid_total += static_cast<uint32_t>(chunk.valid.size());
        mismatch_total += chunk.mismatch;
        biome_total += chunk.biome_reject;
        cap_total += chunk.cap_pruned;
    }

    uint32_t out_idx = 0U;
    for (uint64_t seed : warmup_result.valid) {
        valid_out[out_idx++] = seed;
    }
    for (const BatchChunkResult &chunk : chunk_results) {
        for (uint64_t seed : chunk.valid) {
            valid_out[out_idx++] = seed;
        }
    }

    *valid_count = valid_total;
    *mismatch_count = mismatch_total;
    *biome_reject_count = biome_total;
    *cap_pruned_total = cap_total;
    store_compiled_batch_stats(plan, batch_stats);
    return 0;
}

static int collect_query_details_with_plan(
    const CompiledQueryPlan &plan,
    uint64_t seed,
    int32_t cubi_mc_version,
    NativeQueryMatchOut *match_out,
    uint32_t match_cap,
    uint32_t *match_count,
    NativeQueryBiomeOut *biome_out,
    uint32_t biome_cap,
    uint32_t *biome_count,
    uint32_t *cap_pruned_total
) {
    if (match_count == nullptr || biome_count == nullptr || cap_pruned_total == nullptr ||
        (match_cap > 0U && match_out == nullptr) || (biome_cap > 0U && biome_out == nullptr)) {
        return -1;
    }

    CubiApi api_copy{};
    {
        std::lock_guard<std::mutex> lock(g_cubi_mu);
        api_copy = g_cubi;
    }
    if (plan.direct_api_needed) {
        if (!api_copy.loaded) {
            return -2;
        }
        if (!api_copy.direct_ready) {
            return -3;
        }
    }

    ThreadCubiState cubi_state{};
    QueryScratch scratch(static_cast<uint32_t>(plan.constraints.size()), plan.constraints.empty() ? nullptr : plan.constraints.data());
    std::vector<std::vector<DetailPoint>> detail_by_constraint;
    std::vector<NativeQueryBiomeOut> detail_biome_records;
    uint32_t cap_pruned = 0U;

    const SeedStatus status = collect_query_details_native(
        api_copy,
        cubi_state,
        scratch,
        seed,
        cubi_mc_version,
        plan.regions.empty() ? nullptr : plan.regions.data(),
        static_cast<uint32_t>(plan.regions.size()),
        plan.constraints.empty() ? nullptr : plan.constraints.data(),
        static_cast<uint32_t>(plan.constraints.size()),
        plan.prepared_filters,
        plan.direct_api_needed,
        plan.generator_needed,
        detail_by_constraint,
        detail_biome_records,
        cap_pruned
    );
    if (status != SeedStatus::valid) {
        return status == SeedStatus::biome_reject ? 1 : 1;
    }

    uint32_t mcount = 0U;
    for (uint32_t i = 0; i < detail_by_constraint.size(); ++i) {
        for (const DetailPoint &dp : detail_by_constraint[i]) {
            if (mcount < match_cap && match_out != nullptr) {
                match_out[mcount].constraint_index = i;
                match_out[mcount].x = dp.x;
                match_out[mcount].z = dp.z;
                match_out[mcount].anchor_x = dp.anchor_x;
                match_out[mcount].anchor_z = dp.anchor_z;
                match_out[mcount].dist2 = dist2_i64(dp.x, dp.z, dp.anchor_x, dp.anchor_z);
            }
            ++mcount;
        }
    }

    const uint32_t bcount = static_cast<uint32_t>(detail_biome_records.size());
    for (uint32_t i = 0; i < bcount && i < biome_cap; ++i) {
        if (biome_out != nullptr) {
            biome_out[i] = detail_biome_records[i];
        }
    }

    *match_count = mcount;
    *biome_count = bcount;
    *cap_pruned_total = cap_pruned;
    return 0;
}

static int validate_query_batch_legacy_internal(
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
    uint32_t *cap_pruned_total
) {
    if ((seed_count > 0U && seeds == nullptr) || valid_out == nullptr || valid_count == nullptr ||
        mismatch_count == nullptr || biome_reject_count == nullptr || cap_pruned_total == nullptr) {
        return -1;
    }

    std::vector<QueryBiomePrepared> prepared_filters;
    const int prep_rc = prepare_query_biome_filters(
        biome_filters,
        biome_filter_count,
        biome_allowed_ids,
        biome_allowed_count,
        prepared_filters
    );
    if (prep_rc != 0) {
        return prep_rc;
    }

    const int validate_rc =
        validate_query_descriptors(regions, region_count, constraints, constraint_count, biome_filters, biome_filter_count);
    if (validate_rc != 0) {
        return validate_rc;
    }

    bool direct_api_needed = biome_filter_count > 0U;
    bool generator_needed = biome_filter_count > 0U;
    for (uint32_t i = 0; i < constraint_count; ++i) {
        if (constraint_needs_direct_api(constraints[i])) {
            direct_api_needed = true;
        }
        if (constraint_needs_generator(constraints[i])) {
            generator_needed = true;
        }
    }

    CubiApi api_copy{};
    {
        std::lock_guard<std::mutex> lock(g_cubi_mu);
        api_copy = g_cubi;
    }
    if (direct_api_needed) {
        if (!api_copy.loaded) {
            return -2;
        }
        if (!api_copy.direct_ready) {
            return -3;
        }
    }

    const std::vector<uint32_t> fast_target_caps =
        build_fast_target_caps(constraints, constraint_count, biome_filters, biome_filter_count);
    const std::vector<uint8_t> constraint_store_mask =
        build_constraint_dependency_mask(constraints, constraint_count, biome_filters, biome_filter_count);
    const uint32_t *fast_target_caps_ptr = fast_target_caps.empty() ? nullptr : fast_target_caps.data();
    const std::vector<uint32_t> effective_target_caps =
        build_effective_target_caps(constraints, constraint_count, fast_target_caps_ptr);
    const uint32_t *effective_target_caps_ptr = effective_target_caps.empty() ? nullptr : effective_target_caps.data();
    const uint8_t *constraint_store_mask_ptr =
        constraint_store_mask.empty() ? nullptr : constraint_store_mask.data();
    const std::vector<uint32_t> base_constraint_eval_order =
        build_constraint_eval_order(constraints, constraint_count);
    const uint32_t *base_constraint_eval_order_ptr =
        base_constraint_eval_order.empty() ? nullptr : base_constraint_eval_order.data();
    const std::vector<uint32_t> base_stage0_constraint_eval_order = build_stage0_constraint_eval_order(
        constraints,
        constraint_count,
        base_constraint_eval_order_ptr
    );
    const uint32_t *base_stage0_constraint_eval_order_ptr =
        base_stage0_constraint_eval_order.empty() ? nullptr : base_stage0_constraint_eval_order.data();
    if (constraint_count > 0U &&
        (base_constraint_eval_order_ptr == nullptr || effective_target_caps_ptr == nullptr)) {
        return -1;
    }

    constexpr uint32_t kSelectivityWarmupSeeds = 4096U;
    constexpr uint64_t kSelectivityMinEvalSamples = 32ULL;
    const uint32_t warmup_seed_count = std::min<uint32_t>(seed_count, kSelectivityWarmupSeeds);

    std::vector<ConstraintEvalStats> warmup_stats(constraint_count);
    BatchTaskResult warmup_result;
    warmup_result.valid.reserve(static_cast<size_t>(warmup_seed_count));

    if (warmup_seed_count > 0U) {
        ThreadCubiState warmup_cubi_state{};
        QueryScratch warmup_scratch(constraint_count, constraints);
        ConstraintEvalStats *warmup_stats_ptr = warmup_stats.empty() ? nullptr : warmup_stats.data();
        for (uint32_t i = 0; i < warmup_seed_count; ++i) {
            const SeedStatus stage0_status = evaluate_query_seed_stage0_prefilter(
                seeds[i],
                regions,
                region_count,
                constraints,
                base_stage0_constraint_eval_order_ptr,
                static_cast<uint32_t>(base_stage0_constraint_eval_order.size())
            );
            if (stage0_status != SeedStatus::valid) {
                ++warmup_result.mismatch;
                continue;
            }
            uint32_t cap_pruned = 0U;
            const SeedStatus status = evaluate_query_seed_fast_native_warmup(
                api_copy,
                warmup_cubi_state,
                warmup_scratch,
                seeds[i],
                cubi_mc_version,
                regions,
                region_count,
                constraints,
                constraint_count,
                prepared_filters,
                base_constraint_eval_order_ptr,
                effective_target_caps_ptr,
                constraint_store_mask_ptr,
                direct_api_needed,
                generator_needed,
                cap_pruned,
                warmup_stats_ptr,
                static_cast<uint32_t>(warmup_stats.size())
            );
            warmup_result.cap_pruned += cap_pruned;
            if (status == SeedStatus::valid) {
                warmup_result.valid.push_back(seeds[i]);
            } else if (status == SeedStatus::biome_reject) {
                ++warmup_result.biome_reject;
            } else {
                ++warmup_result.mismatch;
            }
        }
    }

    std::vector<uint32_t> runtime_constraint_eval_order = base_constraint_eval_order;
    if (warmup_seed_count > 0U && constraint_count > 1U) {
        const std::vector<uint32_t> selective_order = build_constraint_eval_order_by_selectivity(
            constraints,
            constraint_count,
            base_constraint_eval_order,
            warmup_stats,
            kSelectivityMinEvalSamples
        );
        if (selective_order.size() == constraint_count) {
            runtime_constraint_eval_order = selective_order;
        }
    }

    const uint32_t *runtime_constraint_eval_order_ptr =
        runtime_constraint_eval_order.empty() ? nullptr : runtime_constraint_eval_order.data();
    const std::vector<uint32_t> runtime_stage0_constraint_eval_order = build_stage0_constraint_eval_order(
        constraints,
        constraint_count,
        runtime_constraint_eval_order_ptr
    );
    const uint32_t *runtime_stage0_constraint_eval_order_ptr =
        runtime_stage0_constraint_eval_order.empty() ? nullptr : runtime_stage0_constraint_eval_order.data();
    if (constraint_count > 0U && runtime_constraint_eval_order_ptr == nullptr) {
        return -1;
    }

    const uint32_t workers = std::max<uint32_t>(1U, worker_count);
    const uint32_t remaining_seed_count = seed_count - warmup_seed_count;
    std::vector<BatchTaskResult> task_results;
    const int par_rc = run_parallel_batch(
        remaining_seed_count,
        workers,
        [&](uint32_t start, uint32_t end, BatchTaskResult &out) {
            ThreadCubiState cubi_state{};
            QueryScratch scratch(constraint_count, constraints);
            out.valid.reserve(static_cast<size_t>(end - start));
            for (uint32_t i = start; i < end; ++i) {
                const uint32_t seed_idx = warmup_seed_count + i;
                const SeedStatus stage0_status = evaluate_query_seed_stage0_prefilter(
                    seeds[seed_idx],
                    regions,
                    region_count,
                    constraints,
                    runtime_stage0_constraint_eval_order_ptr,
                    static_cast<uint32_t>(runtime_stage0_constraint_eval_order.size())
                );
                if (stage0_status != SeedStatus::valid) {
                    ++out.mismatch;
                    continue;
                }
                uint32_t cap_pruned = 0U;
                const SeedStatus status = evaluate_query_seed_fast_native(
                    api_copy,
                    cubi_state,
                    scratch,
                    seeds[seed_idx],
                    cubi_mc_version,
                    regions,
                    region_count,
                    constraints,
                    constraint_count,
                    prepared_filters,
                    runtime_constraint_eval_order_ptr,
                    effective_target_caps_ptr,
                    constraint_store_mask_ptr,
                    direct_api_needed,
                    generator_needed,
                    cap_pruned
                );
                out.cap_pruned += cap_pruned;
                if (status == SeedStatus::valid) {
                    out.valid.push_back(seeds[seed_idx]);
                } else if (status == SeedStatus::biome_reject) {
                    ++out.biome_reject;
                } else {
                    ++out.mismatch;
                }
            }
        },
        task_results
    );
    if (par_rc != 0) {
        return par_rc;
    }

    uint32_t valid_total = static_cast<uint32_t>(warmup_result.valid.size());
    uint32_t mismatch_total = warmup_result.mismatch;
    uint32_t biome_total = warmup_result.biome_reject;
    uint32_t cap_total = warmup_result.cap_pruned;
    for (const BatchTaskResult &task : task_results) {
        valid_total += static_cast<uint32_t>(task.valid.size());
        mismatch_total += task.mismatch;
        biome_total += task.biome_reject;
        cap_total += task.cap_pruned;
    }

    uint32_t out_idx = 0U;
    for (uint64_t seed : warmup_result.valid) {
        valid_out[out_idx++] = seed;
    }
    for (const BatchTaskResult &task : task_results) {
        for (uint64_t seed : task.valid) {
            valid_out[out_idx++] = seed;
        }
    }

    *valid_count = valid_total;
    *mismatch_count = mismatch_total;
    *biome_reject_count = biome_total;
    *cap_pruned_total = cap_total;
    return 0;
}

static int collect_query_details_legacy_internal(
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
    uint32_t *cap_pruned_total
) {
    if (constraints == nullptr || match_count == nullptr || biome_count == nullptr || cap_pruned_total == nullptr) {
        return -1;
    }
    if (region_count > 0U && regions == nullptr) {
        return -1;
    }
    if ((biome_filter_count > 0U && biome_filters == nullptr) || (match_cap > 0U && match_out == nullptr) ||
        (biome_cap > 0U && biome_out == nullptr)) {
        return -1;
    }

    std::vector<QueryBiomePrepared> prepared_filters;
    const int prep_rc = prepare_query_biome_filters(
        biome_filters,
        biome_filter_count,
        biome_allowed_ids,
        biome_allowed_count,
        prepared_filters
    );
    if (prep_rc != 0) {
        return prep_rc;
    }

    const int validate_rc =
        validate_query_descriptors(regions, region_count, constraints, constraint_count, biome_filters, biome_filter_count);
    if (validate_rc != 0) {
        return validate_rc;
    }

    bool direct_api_needed = biome_filter_count > 0U;
    bool generator_needed = biome_filter_count > 0U;
    for (uint32_t i = 0; i < constraint_count; ++i) {
        if (constraint_needs_direct_api(constraints[i])) {
            direct_api_needed = true;
        }
        if (constraint_needs_generator(constraints[i])) {
            generator_needed = true;
        }
    }

    CubiApi api_copy{};
    {
        std::lock_guard<std::mutex> lock(g_cubi_mu);
        api_copy = g_cubi;
    }
    if (direct_api_needed) {
        if (!api_copy.loaded) {
            return -2;
        }
        if (!api_copy.direct_ready) {
            return -3;
        }
    }

    ThreadCubiState cubi_state{};
    QueryScratch scratch(constraint_count, constraints);
    std::vector<std::vector<DetailPoint>> detail_by_constraint;
    std::vector<NativeQueryBiomeOut> detail_biome_records;
    uint32_t cap_pruned = 0U;

    const SeedStatus status = collect_query_details_native(
        api_copy,
        cubi_state,
        scratch,
        seed,
        cubi_mc_version,
        regions,
        region_count,
        constraints,
        constraint_count,
        prepared_filters,
        direct_api_needed,
        generator_needed,
        detail_by_constraint,
        detail_biome_records,
        cap_pruned
    );
    if (status != SeedStatus::valid) {
        return 1;
    }

    uint32_t mcount = 0U;
    for (uint32_t i = 0; i < constraint_count; ++i) {
        for (const DetailPoint &dp : detail_by_constraint[i]) {
            if (mcount < match_cap) {
                match_out[mcount].constraint_index = i;
                match_out[mcount].x = dp.x;
                match_out[mcount].z = dp.z;
                match_out[mcount].anchor_x = dp.anchor_x;
                match_out[mcount].anchor_z = dp.anchor_z;
                match_out[mcount].dist2 = dist2_i64(dp.x, dp.z, dp.anchor_x, dp.anchor_z);
            }
            ++mcount;
        }
    }

    const uint32_t bcount = static_cast<uint32_t>(detail_biome_records.size());
    for (uint32_t i = 0; i < bcount && i < biome_cap; ++i) {
        biome_out[i] = detail_biome_records[i];
    }

    *match_count = mcount;
    *biome_count = bcount;
    *cap_pruned_total = cap_pruned;
    return 0;
}

} // namespace

extern "C" {

SCANNER_CORE_API int scanner_core_is_available(void) {
    return 1;
}

SCANNER_CORE_API int scanner_core_set_cubiomes_lib_path(const char *path_utf8) {
    std::lock_guard<std::mutex> lock(g_cubi_mu);
    return load_cubi_locked(path_utf8) ? 1 : 0;
}

SCANNER_CORE_API ScannerCompiledQueryPlan *scanner_core_compile_query_plan(
    const NativeRegionTerm *regions,
    uint32_t region_count,
    const NativeQueryConstraintDesc *constraints,
    uint32_t constraint_count,
    const NativeBiomeFilterDesc *biome_filters,
    uint32_t biome_filter_count,
    const int32_t *biome_allowed_ids,
    uint32_t biome_allowed_count
) {
    int rc = 0;
    std::unique_ptr<CompiledQueryPlan> plan = compile_query_plan_internal(
        regions,
        region_count,
        constraints,
        constraint_count,
        biome_filters,
        biome_filter_count,
        biome_allowed_ids,
        biome_allowed_count,
        rc
    );
    if (rc != 0 || !plan) {
        return nullptr;
    }
    ScannerCompiledQueryPlan *handle = new ScannerCompiledQueryPlan{};
    handle->impl = plan.release();
    return handle;
}

SCANNER_CORE_API void scanner_core_free_query_plan(ScannerCompiledQueryPlan *plan) {
    if (plan == nullptr) {
        return;
    }
    delete unwrap_query_plan(plan);
    delete plan;
}

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
    uint32_t *cap_pruned_total
) {
    const CompiledQueryPlan *compiled = unwrap_query_plan(plan);
    if (compiled == nullptr) {
        return -1;
    }
    return validate_query_batch_with_plan(
        *compiled,
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

SCANNER_CORE_API int scanner_core_validate_query_batch_compiled_post_stage_a(
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
) {
    const CompiledQueryPlan *compiled = unwrap_query_plan(plan);
    if (compiled == nullptr) {
        return -1;
    }
    return validate_query_batch_with_plan(
        *compiled,
        seeds,
        seed_count,
        cubi_mc_version,
        worker_count,
        valid_out,
        valid_count,
        mismatch_count,
        biome_reject_count,
        cap_pruned_total,
        true
    );
}

SCANNER_CORE_API int scanner_core_get_compiled_batch_stats(
    const ScannerCompiledQueryPlan *plan,
    ScannerCompiledBatchStats *stats
) {
    const CompiledQueryPlan *compiled = unwrap_query_plan(plan);
    if (compiled == nullptr || stats == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(compiled->batch_stats_mu);
    *stats = compiled->last_batch_stats;
    return 0;
}

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
    uint32_t *cap_pruned_total
) {
    const CompiledQueryPlan *compiled = unwrap_query_plan(plan);
    if (compiled == nullptr) {
        return -1;
    }
    return collect_query_details_with_plan(
        *compiled,
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
    uint32_t *biome_reject_count
) {
    if ((seed_count > 0U && seeds == nullptr) || regions == nullptr || contexts == nullptr || valid_out == nullptr ||
        valid_count == nullptr || mismatch_count == nullptr || biome_reject_count == nullptr) {
        return -1;
    }
    if (context_count == 0U) {
        *valid_count = 0U;
        *mismatch_count = seed_count;
        *biome_reject_count = 0U;
        return 0;
    }
    for (uint32_t i = 0; i < context_count; ++i) {
        const NativeLegacyContextDesc &ctx = contexts[i];
        if (ctx.region_start > region_count || ctx.region_start + ctx.region_count > region_count) {
            return -1;
        }
    }

    LegacyBiomePrepared biome{};
    if (biome_desc != nullptr && biome_desc->enabled != 0U) {
        biome.enabled = true;
        biome.point_type = biome_desc->point_type;
        biome.y = biome_desc->y;
        biome.radius = std::max<int32_t>(0, biome_desc->radius);
        if (biome_allowed_ids == nullptr) {
            return -1;
        }
        if (biome_desc->allowed_start > biome_allowed_count ||
            biome_desc->allowed_start + biome_desc->allowed_count > biome_allowed_count) {
            return -1;
        }
        const int32_t *slice = biome_allowed_ids + biome_desc->allowed_start;
        biome.allowed_sorted = sorted_unique_ids(slice, biome_desc->allowed_count);
        if (biome.allowed_sorted.empty()) {
            return -1;
        }
        biome.offsets = &build_biome_offsets_cached(biome.radius, 4);
    }

    const bool need_cubi = strict_viability != 0U || biome.enabled;
    CubiApi api_copy{};
    {
        std::lock_guard<std::mutex> lock(g_cubi_mu);
        api_copy = g_cubi;
    }
    if (need_cubi) {
        if (!api_copy.loaded) {
            return -2;
        }
        if (!api_copy.direct_ready) {
            return -3;
        }
    }

    const uint32_t workers = std::max<uint32_t>(1U, worker_count);
    std::vector<BatchTaskResult> task_results;
    const int par_rc = run_parallel_batch(
        seed_count,
        workers,
        [&](uint32_t start, uint32_t end, BatchTaskResult &out) {
            ThreadCubiState cubi_state{};
            out.valid.reserve(static_cast<size_t>(end - start));
            for (uint32_t i = start; i < end; ++i) {
                const SeedStatus st = evaluate_legacy_seed(
                    api_copy,
                    cubi_state,
                    seeds[i],
                    regions,
                    contexts,
                    context_count,
                    radius_sq,
                    strict_viability != 0U,
                    cubi_mc_version,
                    biome
                );
                if (st == SeedStatus::valid) {
                    out.valid.push_back(seeds[i]);
                } else if (st == SeedStatus::biome_reject) {
                    ++out.biome_reject;
                } else {
                    ++out.mismatch;
                }
            }
        },
        task_results
    );
    if (par_rc != 0) {
        return par_rc;
    }

    uint32_t valid_total = 0U;
    uint32_t mismatch_total = 0U;
    uint32_t biome_total = 0U;
    for (const BatchTaskResult &t : task_results) {
        valid_total += static_cast<uint32_t>(t.valid.size());
        mismatch_total += t.mismatch;
        biome_total += t.biome_reject;
    }

    uint32_t out_idx = 0U;
    for (const BatchTaskResult &t : task_results) {
        for (uint64_t seed : t.valid) {
            valid_out[out_idx++] = seed;
        }
    }

    *valid_count = valid_total;
    *mismatch_count = mismatch_total;
    *biome_reject_count = biome_total;
    return 0;
}

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
    uint32_t *cap_pruned_total
) {
    int compile_rc = 0;
    std::unique_ptr<CompiledQueryPlan> plan = compile_query_plan_internal(
        regions,
        region_count,
        constraints,
        constraint_count,
        biome_filters,
        biome_filter_count,
        biome_allowed_ids,
        biome_allowed_count,
        compile_rc
    );
    if (compile_rc != 0 || !plan) {
        return compile_rc != 0 ? compile_rc : -1;
    }
    return validate_query_batch_with_plan(
        *plan,
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
    uint32_t *cap_pruned_total
) {
    return validate_query_batch_legacy_internal(
        seeds,
        seed_count,
        regions,
        region_count,
        constraints,
        constraint_count,
        biome_filters,
        biome_filter_count,
        biome_allowed_ids,
        biome_allowed_count,
        cubi_mc_version,
        worker_count,
        valid_out,
        valid_count,
        mismatch_count,
        biome_reject_count,
        cap_pruned_total
    );
}

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
    uint32_t *reject_count
) {
    if ((seed_count > 0U && seeds == nullptr) || sculpt_desc == nullptr || valid_out == nullptr ||
        valid_count == nullptr || reject_count == nullptr) {
        return -1;
    }

    if (sculpt_desc->enabled == 0U) {
        for (uint32_t i = 0; i < seed_count; ++i) {
            valid_out[i] = seeds[i];
        }
        *valid_count = seed_count;
        *reject_count = 0U;
        return 0;
    }

    if (allowed_ids == nullptr || allowed_count == 0U) {
        return -1;
    }
    if (sculpt_desc->pattern > SCAN_SCULPT_CROSS) {
        return -1;
    }
    if (sculpt_desc->edge_mode != SCAN_EDGE_OUTWARD && sculpt_desc->edge_mode != SCAN_EDGE_INWARD) {
        return -1;
    }
    if (sculpt_desc->center_type != SCAN_ANCHOR_ORIGIN && sculpt_desc->center_type != SCAN_ANCHOR_SPAWN &&
        sculpt_desc->center_type != SCAN_ANCHOR_FIXED) {
        return -1;
    }
    if (sculpt_desc->radius < 0 || sculpt_desc->step <= 0 || !std::isfinite(sculpt_desc->dominance_threshold) ||
        sculpt_desc->dominance_threshold < 0.0 || sculpt_desc->dominance_threshold > 1.0) {
        return -1;
    }

    const std::vector<SculptOffset> &sculpt_offsets =
        build_sculpt_offsets_cached(sculpt_desc->pattern, sculpt_desc->radius, sculpt_desc->step);
    if (sculpt_offsets.empty()) {
        return -1;
    }
    const uint32_t total_samples = static_cast<uint32_t>(sculpt_offsets.size());
    const uint32_t required_hits_raw = static_cast<uint32_t>(
        std::ceil(sculpt_desc->dominance_threshold * static_cast<double>(total_samples))
    );
    const uint32_t required_hits = std::min<uint32_t>(required_hits_raw, total_samples);
    const uint32_t max_misses = total_samples - required_hits;

    if (required_hits == 0U) {
        for (uint32_t i = 0; i < seed_count; ++i) {
            valid_out[i] = seeds[i];
        }
        *valid_count = seed_count;
        *reject_count = 0U;
        return 0;
    }

    const std::vector<int32_t> allowed_sorted = sorted_unique_ids(allowed_ids, allowed_count);
    if (allowed_sorted.empty()) {
        return -1;
    }

    CubiApi api_copy{};
    {
        std::lock_guard<std::mutex> lock(g_cubi_mu);
        api_copy = g_cubi;
    }
    if (!api_copy.loaded) {
        return -2;
    }
    if (!api_copy.direct_ready) {
        return -3;
    }

    if (seed_count == 0U) {
        *valid_count = 0U;
        *reject_count = 0U;
        return 0;
    }

    const uint32_t workers = std::max<uint32_t>(1U, worker_count);
    std::vector<BatchTaskResult> task_results;
    const int par_rc = run_parallel_batch(
        seed_count,
        workers,
        [&](uint32_t start, uint32_t end, BatchTaskResult &out) {
            ThreadCubiState cubi_state{};
            out.valid.reserve(static_cast<size_t>(end - start));
            for (uint32_t i = start; i < end; ++i) {
                if (evaluate_sculpt_seed(
                        api_copy,
                        cubi_state,
                        seeds[i],
                        cubi_mc_version,
                        *sculpt_desc,
                        sculpt_offsets,
                        required_hits,
                        max_misses,
                        allowed_sorted)) {
                    out.valid.push_back(seeds[i]);
                } else {
                    ++out.biome_reject;
                }
            }
        },
        task_results
    );
    if (par_rc != 0) {
        return par_rc;
    }

    uint32_t valid_total = 0U;
    uint32_t reject_total = 0U;
    for (const BatchTaskResult &t : task_results) {
        valid_total += static_cast<uint32_t>(t.valid.size());
        reject_total += t.biome_reject;
    }

    uint32_t out_idx = 0U;
    for (const BatchTaskResult &t : task_results) {
        for (uint64_t seed : t.valid) {
            valid_out[out_idx++] = seed;
        }
    }

    *valid_count = valid_total;
    *reject_count = reject_total;
    return 0;
}

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
    uint32_t *cap_pruned_total
) {
    if (constraints == nullptr || match_count == nullptr || biome_count == nullptr || cap_pruned_total == nullptr) {
        return -1;
    }
    if (region_count > 0U && regions == nullptr) {
        return -1;
    }
    if ((biome_filter_count > 0U && biome_filters == nullptr) || (match_cap > 0U && match_out == nullptr) ||
        (biome_cap > 0U && biome_out == nullptr)) {
        return -1;
    }
    int compile_rc = 0;
    std::unique_ptr<CompiledQueryPlan> plan = compile_query_plan_internal(
        regions,
        region_count,
        constraints,
        constraint_count,
        biome_filters,
        biome_filter_count,
        biome_allowed_ids,
        biome_allowed_count,
        compile_rc
    );
    if (compile_rc != 0 || !plan) {
        return compile_rc != 0 ? compile_rc : -1;
    }
    return collect_query_details_with_plan(
        *plan,
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
    uint32_t *cap_pruned_total
) {
    return collect_query_details_legacy_internal(
        seed,
        regions,
        region_count,
        constraints,
        constraint_count,
        biome_filters,
        biome_filter_count,
        biome_allowed_ids,
        biome_allowed_count,
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

} // extern "C"
