#include "gpu_filter.h"

#include <cuda_runtime.h>
#include <stdint.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t JAVA_MULT = 25214903917ULL;
constexpr uint64_t JAVA_ADD = 11ULL;
constexpr uint64_t JAVA_MASK = (1ULL << 48) - 1ULL;
constexpr uint64_t INT32_MASK = 0xFFFFFFFFULL;
constexpr uint64_t INT32_SIGN = 0x80000000ULL;
constexpr int CHUNK_SIZE = 16;

constexpr uint32_t SPREAD_LINEAR = 0;
constexpr uint32_t SPREAD_TRIANGULAR = 1;

constexpr int THREADS_PER_BLOCK = 256;
// Each thread processes SEEDS_PER_THREAD seeds in 4-way ILP. Bigger launches
// amortize host-launch overhead and let the GPU run thousands of blocks per SM.
constexpr int SEEDS_PER_THREAD = 4;
constexpr uint64_t MAX_SEEDS_PER_LAUNCH = 64ULL * 1024ULL * 1024ULL;
constexpr int MAX_CONST_REGIONS = 2048;
constexpr uint32_t MAX_QUAD_CHECK_POINTS = 128U;

enum GpuFilterStatus : int {
    GPU_FILTER_STATUS_OK = 0,
    GPU_FILTER_STATUS_INVALID_ARG = -1,
    GPU_FILTER_STATUS_NO_DEVICE = -2,
    GPU_FILTER_STATUS_REGION_LIMIT = -3,
    GPU_FILTER_STATUS_DEVICE_SETUP = -4,
    GPU_FILTER_STATUS_ALLOC = -5,
    GPU_FILTER_STATUS_COPY = -6,
    GPU_FILTER_STATUS_LAUNCH = -7,
};

__constant__ RegionTerm k_regions[MAX_CONST_REGIONS];
// k_constraints intentionally omitted: combined with k_regions[2048] it exceeds
// the 64 KB const-memory cap. Constraint descriptors live in global memory and
// are loaded uniformly via __ldg / L1 (broadcast to all threads in a warp).

struct DeviceWorkspace {
    uint64_t *d_out = nullptr;
    uint32_t *d_hit = nullptr;
    ConstraintDesc *d_constraints = nullptr;
    uint64_t out_capacity = 0ULL;
    uint32_t constraint_capacity = 0U;
    uint64_t regions_hash = 0ULL;
    uint32_t regions_count = 0U;
    uint64_t constraints_hash = 0ULL;
    uint32_t constraints_count = 0U;
    cudaStream_t stream = nullptr;
    bool stream_ready = false;
};

inline cudaStream_t ensure_stream(DeviceWorkspace &workspace) {
    if (!workspace.stream_ready) {
        if (cudaStreamCreateWithFlags(&workspace.stream, cudaStreamNonBlocking) == cudaSuccess) {
            workspace.stream_ready = true;
        } else {
            workspace.stream = nullptr;
        }
    }
    return workspace.stream;
}

std::mutex g_workspace_mu;
std::vector<DeviceWorkspace> g_workspaces;

int gpu_device_count_impl() {
    int count = 0;
    const cudaError_t st = cudaGetDeviceCount(&count);
    return (st == cudaSuccess && count > 0) ? count : 0;
}

DeviceWorkspace &workspace_for_device(int device_index) {
    if (device_index < 0) {
        device_index = 0;
    }
    if (static_cast<size_t>(device_index) >= g_workspaces.size()) {
        g_workspaces.resize(static_cast<size_t>(device_index) + 1U);
    }
    return g_workspaces[static_cast<size_t>(device_index)];
}

__host__ __forceinline__ uint64_t fnv1a64_append(uint64_t h, const void *data, size_t bytes) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

__host__ __forceinline__ uint64_t hash_regions(const RegionTerm *regions, uint32_t region_count) {
    uint64_t h = 1469598103934665603ULL;
    h = fnv1a64_append(h, &region_count, sizeof(region_count));
    if (regions != nullptr && region_count > 0U) {
        h = fnv1a64_append(h, regions, static_cast<size_t>(region_count) * sizeof(RegionTerm));
    }
    return h;
}

__host__ __forceinline__ uint64_t hash_constraints(const ConstraintDesc *constraints, uint32_t constraint_count) {
    uint64_t h = 1469598103934665603ULL;
    h = fnv1a64_append(h, &constraint_count, sizeof(constraint_count));
    if (constraints != nullptr && constraint_count > 0U) {
        h = fnv1a64_append(h, constraints, static_cast<size_t>(constraint_count) * sizeof(ConstraintDesc));
    }
    return h;
}

bool ensure_output_capacity(DeviceWorkspace &workspace, uint64_t count) {
    if (workspace.d_out != nullptr && workspace.d_hit != nullptr && workspace.out_capacity >= count) {
        return true;
    }

    uint64_t new_cap = count;
    if (workspace.out_capacity > 0ULL) {
        const uint64_t grown = workspace.out_capacity + workspace.out_capacity / 2ULL;
        if (grown > new_cap) {
            new_cap = grown;
        }
    }
    new_cap = std::max<uint64_t>(1ULL, new_cap);

    uint64_t *new_out = nullptr;
    uint32_t *new_hit = nullptr;
    if (cudaMalloc(&new_out, static_cast<size_t>(new_cap) * sizeof(uint64_t)) != cudaSuccess) {
        return false;
    }
    if (cudaMalloc(&new_hit, sizeof(uint32_t)) != cudaSuccess) {
        cudaFree(new_out);
        return false;
    }

    if (workspace.d_out != nullptr) {
        cudaFree(workspace.d_out);
    }
    if (workspace.d_hit != nullptr) {
        cudaFree(workspace.d_hit);
    }
    workspace.d_out = new_out;
    workspace.d_hit = new_hit;
    workspace.out_capacity = new_cap;
    return true;
}

bool ensure_constraint_capacity(DeviceWorkspace &workspace, uint32_t constraint_count) {
    if (constraint_count == 0U) {
        return true;
    }
    if (workspace.d_constraints != nullptr && workspace.constraint_capacity >= constraint_count) {
        return true;
    }

    uint32_t new_cap = constraint_count;
    if (workspace.constraint_capacity > 0U) {
        const uint32_t grown = workspace.constraint_capacity + workspace.constraint_capacity / 2U;
        if (grown > new_cap) {
            new_cap = grown;
        }
    }
    new_cap = std::max<uint32_t>(1U, new_cap);

    ConstraintDesc *new_constraints = nullptr;
    if (cudaMalloc(&new_constraints, static_cast<size_t>(new_cap) * sizeof(ConstraintDesc)) != cudaSuccess) {
        return false;
    }

    if (workspace.d_constraints != nullptr) {
        cudaFree(workspace.d_constraints);
    }
    workspace.d_constraints = new_constraints;
    workspace.constraint_capacity = new_cap;
    workspace.constraints_hash = 0ULL;
    workspace.constraints_count = 0U;
    return true;
}

__device__ __forceinline__ bool is_pow2_u32(uint32_t v) {
    return v != 0 && (v & (v - 1U)) == 0U;
}

__device__ __forceinline__ uint64_t lcg_step(uint64_t state) {
    return (state * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
}

__device__ __forceinline__ uint32_t next_int_exact(uint64_t &state, uint32_t bound) {
    state = lcg_step(state);
    if (is_pow2_u32(bound)) {
        return static_cast<uint32_t>((static_cast<uint64_t>(bound) * (state >> 17)) >> 31);
    }

    uint32_t bits = static_cast<uint32_t>(state >> 17);
    uint32_t val = static_cast<uint32_t>(bits % bound);
    while ((bits - val + (bound - 1U)) >= INT32_SIGN) {
        state = lcg_step(state);
        bits = static_cast<uint32_t>(state >> 17);
        val = static_cast<uint32_t>(bits % bound);
    }
    return val;
}

__device__ __forceinline__ bool region_hit_exact(
    const RegionTerm &term,
    uint64_t seed48,
    int32_t anchor_x,
    int32_t anchor_z,
    uint64_t radius_sq,
    int32_t *out_x = nullptr,
    int32_t *out_z = nullptr) {
    uint64_t state = ((seed48 + term.add_term_mod48) ^ JAVA_MULT) & JAVA_MASK;
    const uint32_t bound = term.bound;
    if (bound == 0U) {
        return false;
    }

    uint32_t off_x = 0U;
    uint32_t off_z = 0U;
    if (term.spread_type == SPREAD_TRIANGULAR) {
        const uint32_t x1 = next_int_exact(state, bound);
        const uint32_t x2 = next_int_exact(state, bound);
        const uint32_t z1 = next_int_exact(state, bound);
        const uint32_t z2 = next_int_exact(state, bound);
        off_x = (x1 + x2) >> 1;
        off_z = (z1 + z2) >> 1;
    } else {
        off_x = next_int_exact(state, bound);
        off_z = next_int_exact(state, bound);
    }

    const int64_t bx = static_cast<int64_t>(term.base_block_x) + static_cast<int64_t>(off_x) * CHUNK_SIZE;
    const int64_t bz = static_cast<int64_t>(term.base_block_z) + static_cast<int64_t>(off_z) * CHUNK_SIZE;
    const int64_t dx = bx - static_cast<int64_t>(anchor_x);
    const int64_t dz = bz - static_cast<int64_t>(anchor_z);
    const uint64_t d2 = static_cast<uint64_t>(dx * dx + dz * dz);
    if (d2 > radius_sq) {
        return false;
    }
    if (out_x != nullptr) {
        *out_x = static_cast<int32_t>(bx);
    }
    if (out_z != nullptr) {
        *out_z = static_cast<int32_t>(bz);
    }
    return true;
}

__device__ __forceinline__ uint32_t next_int_fast(uint64_t &state, uint32_t bound) {
    state = lcg_step(state);
    if (is_pow2_u32(bound)) {
        return static_cast<uint32_t>((static_cast<uint64_t>(bound) * (state >> 17)) >> 31);
    }
    return static_cast<uint32_t>(state >> 17) % bound;
}

__device__ __forceinline__ bool gate_pass(uint64_t seed48, uint32_t gate_div, uint32_t gate_salt) {
    if (gate_div <= 1U) {
        return true;
    }
    const uint64_t mixed = seed48 + (static_cast<uint64_t>(gate_salt) & JAVA_MASK);
    if (is_pow2_u32(gate_div)) {
        return (mixed & (static_cast<uint64_t>(gate_div) - 1ULL)) == 0ULL;
    }
    return (mixed % gate_div) == 0ULL;
}

__device__ __forceinline__ bool region_hit(
    const RegionTerm &term,
    uint64_t seed48,
    int32_t anchor_x,
    int32_t anchor_z,
    uint64_t radius_sq,
    int32_t *out_x = nullptr,
    int32_t *out_z = nullptr) {
    uint64_t state = ((seed48 + term.add_term_mod48) ^ JAVA_MULT) & JAVA_MASK;
    const bool use_fast = term.use_fast_next_int != 0U;
    const uint32_t bound = term.bound;
    if (bound == 0U) {
        return false;
    }

    uint32_t off_x = 0U;
    uint32_t off_z = 0U;
    if (term.spread_type == SPREAD_TRIANGULAR) {
        const uint32_t x1 = use_fast ? next_int_fast(state, bound) : next_int_exact(state, bound);
        const uint32_t x2 = use_fast ? next_int_fast(state, bound) : next_int_exact(state, bound);
        const uint32_t z1 = use_fast ? next_int_fast(state, bound) : next_int_exact(state, bound);
        const uint32_t z2 = use_fast ? next_int_fast(state, bound) : next_int_exact(state, bound);
        off_x = (x1 + x2) >> 1;
        off_z = (z1 + z2) >> 1;
    } else {
        off_x = use_fast ? next_int_fast(state, bound) : next_int_exact(state, bound);
        off_z = use_fast ? next_int_fast(state, bound) : next_int_exact(state, bound);
    }

    const int64_t bx = static_cast<int64_t>(term.base_block_x) + static_cast<int64_t>(off_x) * CHUNK_SIZE;
    const int64_t bz = static_cast<int64_t>(term.base_block_z) + static_cast<int64_t>(off_z) * CHUNK_SIZE;
    const int64_t dx = bx - static_cast<int64_t>(anchor_x);
    const int64_t dz = bz - static_cast<int64_t>(anchor_z);
    const uint64_t d2 = static_cast<uint64_t>(dx * dx + dz * dz);
    if (d2 > radius_sq) {
        return false;
    }
    if (out_x != nullptr) {
        *out_x = static_cast<int32_t>(bx);
    }
    if (out_z != nullptr) {
        *out_z = static_cast<int32_t>(bz);
    }
    return true;
}

__device__ __forceinline__ bool has_compact_group_within_span(
    const int32_t *xs,
    const int32_t *zs,
    uint32_t count,
    uint32_t min_required,
    uint32_t max_span) {
    if (count < min_required) {
        return false;
    }
    if (min_required <= 1U || max_span == 0U) {
        return true;
    }

    const int64_t span = static_cast<int64_t>(max_span);
    for (uint32_t xi = 0; xi < count; ++xi) {
        const int64_t min_x = static_cast<int64_t>(xs[xi]);
        const int64_t max_x = min_x + span;
        for (uint32_t zi = 0; zi < count; ++zi) {
            const int64_t min_z = static_cast<int64_t>(zs[zi]);
            const int64_t max_z = min_z + span;
            uint32_t inside = 0U;
            for (uint32_t k = 0; k < count; ++k) {
                const int64_t x = static_cast<int64_t>(xs[k]);
                const int64_t z = static_cast<int64_t>(zs[k]);
                if (x < min_x || x > max_x || z < min_z || z > max_z) {
                    continue;
                }
                ++inside;
                if (inside >= min_required) {
                    return true;
                }
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 4-way ILP helpers. Each thread runs N independent LCG state chains so that
// the GPU's pipelined integer multiply (~10-20 cycle latency on Ampere/Ada)
// is kept full. The per-chain dependency is unchanged — output is identical
// to the scalar path.
// ---------------------------------------------------------------------------

template<int N>
__device__ __forceinline__ void lcg_step_n(uint64_t (&state)[N]) {
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        state[i] = (state[i] * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
    }
}

template<int N>
__device__ __forceinline__ void next_int_exact_n(
    uint64_t (&state)[N], uint32_t bound, bool pow2, uint32_t (&out)[N]) {
    lcg_step_n<N>(state);
    if (pow2) {
        #pragma unroll
        for (int i = 0; i < N; ++i) {
            out[i] = static_cast<uint32_t>((static_cast<uint64_t>(bound) * (state[i] >> 17)) >> 31);
        }
    } else {
        #pragma unroll
        for (int i = 0; i < N; ++i) {
            uint32_t bits = static_cast<uint32_t>(state[i] >> 17);
            uint32_t val = bits % bound;
            // Java nextInt rejection. For typical small bounds the rejection
            // probability is on the order of 2^-26 so this loop almost never
            // iterates and the compiler keeps the four chains pipelined.
            while ((bits - val + (bound - 1U)) >= INT32_SIGN) {
                state[i] = (state[i] * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
                bits = static_cast<uint32_t>(state[i] >> 17);
                val = bits % bound;
            }
            out[i] = val;
        }
    }
}

template<int N>
__device__ __forceinline__ void region_offsets_n(
    const RegionTerm &term,
    const uint64_t (&seed48)[N],
    uint32_t (&off_x)[N],
    uint32_t (&off_z)[N]) {
    const uint32_t bound = term.bound;
    const bool pow2 = is_pow2_u32(bound);
    const uint64_t add = term.add_term_mod48;

    uint64_t state[N];
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        state[i] = ((seed48[i] + add) ^ JAVA_MULT) & JAVA_MASK;
    }

    if (term.spread_type == SPREAD_TRIANGULAR) {
        uint32_t x1[N], x2[N], z1[N], z2[N];
        next_int_exact_n<N>(state, bound, pow2, x1);
        next_int_exact_n<N>(state, bound, pow2, x2);
        next_int_exact_n<N>(state, bound, pow2, z1);
        next_int_exact_n<N>(state, bound, pow2, z2);
        #pragma unroll
        for (int i = 0; i < N; ++i) {
            off_x[i] = (x1[i] + x2[i]) >> 1;
            off_z[i] = (z1[i] + z2[i]) >> 1;
        }
    } else {
        next_int_exact_n<N>(state, bound, pow2, off_x);
        next_int_exact_n<N>(state, bound, pow2, off_z);
    }
}

template<int N>
__device__ __forceinline__ void region_hit_n(
    const RegionTerm &term,
    const uint64_t (&seed48)[N],
    int32_t anchor_x,
    int32_t anchor_z,
    uint64_t radius_sq,
    bool (&hit)[N]) {
    if (term.bound == 0U) {
        #pragma unroll
        for (int i = 0; i < N; ++i) {
            hit[i] = false;
        }
        return;
    }
    uint32_t off_x[N], off_z[N];
    region_offsets_n<N>(term, seed48, off_x, off_z);
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        const int64_t bx = static_cast<int64_t>(term.base_block_x) + static_cast<int64_t>(off_x[i]) * CHUNK_SIZE;
        const int64_t bz = static_cast<int64_t>(term.base_block_z) + static_cast<int64_t>(off_z[i]) * CHUNK_SIZE;
        const int64_t dx = bx - static_cast<int64_t>(anchor_x);
        const int64_t dz = bz - static_cast<int64_t>(anchor_z);
        const uint64_t d2 = static_cast<uint64_t>(dx * dx + dz * dz);
        hit[i] = (d2 <= radius_sq);
    }
}

template<int N>
__device__ __forceinline__ void region_hit_n_with_pos(
    const RegionTerm &term,
    const uint64_t (&seed48)[N],
    int32_t anchor_x,
    int32_t anchor_z,
    uint64_t radius_sq,
    bool (&hit)[N],
    int32_t (&out_x)[N],
    int32_t (&out_z)[N]) {
    if (term.bound == 0U) {
        #pragma unroll
        for (int i = 0; i < N; ++i) {
            hit[i] = false;
        }
        return;
    }
    uint32_t off_x[N], off_z[N];
    region_offsets_n<N>(term, seed48, off_x, off_z);
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        const int64_t bx = static_cast<int64_t>(term.base_block_x) + static_cast<int64_t>(off_x[i]) * CHUNK_SIZE;
        const int64_t bz = static_cast<int64_t>(term.base_block_z) + static_cast<int64_t>(off_z[i]) * CHUNK_SIZE;
        const int64_t dx = bx - static_cast<int64_t>(anchor_x);
        const int64_t dz = bz - static_cast<int64_t>(anchor_z);
        const uint64_t d2 = static_cast<uint64_t>(dx * dx + dz * dz);
        hit[i] = (d2 <= radius_sq);
        out_x[i] = static_cast<int32_t>(bx);
        out_z[i] = static_cast<int32_t>(bz);
    }
}

template<int N>
__device__ __forceinline__ void gate_pass_n(
    const uint64_t (&seed48)[N], uint32_t gate_div, uint32_t gate_salt, bool (&pass)[N]) {
    if (gate_div <= 1U) {
        #pragma unroll
        for (int i = 0; i < N; ++i) pass[i] = true;
        return;
    }
    const bool pow2 = is_pow2_u32(gate_div);
    const uint64_t mask = static_cast<uint64_t>(gate_div) - 1ULL;
    const uint64_t add = static_cast<uint64_t>(gate_salt) & JAVA_MASK;
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        const uint64_t mixed = seed48[i] + add;
        pass[i] = pow2 ? ((mixed & mask) == 0ULL) : ((mixed % gate_div) == 0ULL);
    }
}

template<int N>
__device__ __forceinline__ void emit_hits_n(
    const bool (&hit)[N],
    const uint64_t (&seed48)[N],
    uint64_t *out,
    uint32_t *hit_count,
    uint64_t out_capacity) {
    uint32_t mask = 0U;
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        if (hit[i]) mask |= (1U << i);
    }
    if (mask == 0U) return;
    const uint32_t popcnt = __popc(mask);
    const uint32_t base = atomicAdd(hit_count, popcnt);
    uint32_t slot = 0U;
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        if (hit[i]) {
            const uint64_t pos = static_cast<uint64_t>(base) + static_cast<uint64_t>(slot);
            if (pos < out_capacity) {
                out[pos] = seed48[i];
            }
            ++slot;
        }
    }
}

// ---------------------------------------------------------------------------
// ILP-4 kernels. Each thread covers SEEDS_PER_THREAD consecutive seeds.
// ---------------------------------------------------------------------------

__global__ void filter_one_min1_kernel_x4(
    uint64_t start_seed,
    uint64_t count,
    uint64_t radius_sq,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t region_start,
    uint32_t region_count,
    uint64_t *out,
    uint32_t *hit_count) {
    constexpr int N = SEEDS_PER_THREAD;
    const uint64_t tid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const uint64_t base_idx = tid * static_cast<uint64_t>(N);
    if (base_idx >= count) return;

    uint64_t seed48[N];
    bool active[N];
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        const uint64_t idx = base_idx + static_cast<uint64_t>(i);
        active[i] = (idx < count);
        seed48[i] = (start_seed + idx) & JAVA_MASK;
    }

    bool any_hit[N];
    #pragma unroll
    for (int i = 0; i < N; ++i) any_hit[i] = false;

    const uint32_t end = region_start + region_count;
    for (uint32_t ri = region_start; ri < end; ++ri) {
        const RegionTerm term = k_regions[ri];
        bool hit[N];
        int32_t bx[N], bz[N];
        region_hit_n_with_pos<N>(term, seed48, anchor_x, anchor_z, radius_sq, hit, bx, bz);
        #pragma unroll
        for (int i = 0; i < N; ++i) {
            any_hit[i] = any_hit[i] || (hit[i] && active[i]);
        }
    }

    #pragma unroll
    for (int i = 0; i < N; ++i) {
        any_hit[i] = any_hit[i] && active[i];
    }
    emit_hits_n<N>(any_hit, seed48, out, hit_count, count);
}

__global__ void filter_single_kernel_x4(
    uint64_t start_seed,
    uint64_t count,
    uint64_t radius_sq,
    uint32_t gate_div,
    uint32_t gate_salt,
    uint64_t *out,
    uint32_t *hit_count,
    uint32_t region_count) {
    constexpr int N = SEEDS_PER_THREAD;
    const uint64_t tid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const uint64_t base_idx = tid * static_cast<uint64_t>(N);
    if (base_idx >= count) return;

    uint64_t seed48[N];
    bool active[N];
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        const uint64_t idx = base_idx + static_cast<uint64_t>(i);
        active[i] = (idx < count);
        seed48[i] = (start_seed + idx) & JAVA_MASK;
    }

    bool gate[N];
    gate_pass_n<N>(seed48, gate_div, gate_salt, gate);
    bool any_hit[N];
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        any_hit[i] = false;
        active[i] = active[i] && gate[i];
    }

    for (uint32_t ri = 0; ri < region_count; ++ri) {
        const RegionTerm term = k_regions[ri];
        bool hit[N];
        int32_t bx[N], bz[N];
        region_hit_n_with_pos<N>(term, seed48, 0, 0, radius_sq, hit, bx, bz);
        #pragma unroll
        for (int i = 0; i < N; ++i) {
            any_hit[i] = any_hit[i] || (hit[i] && active[i]);
        }
    }
    emit_hits_n<N>(any_hit, seed48, out, hit_count, count);
}

__device__ __forceinline__ bool has_compact_group_within_span_local(
    const int32_t *xs,
    const int32_t *zs,
    uint32_t n,
    uint32_t min_required,
    uint32_t max_span) {
    if (n < min_required) return false;
    if (min_required <= 1U || max_span == 0U) return true;
    const int64_t span = static_cast<int64_t>(max_span);
    for (uint32_t xi = 0; xi < n; ++xi) {
        const int64_t min_x = static_cast<int64_t>(xs[xi]);
        const int64_t max_x = min_x + span;
        for (uint32_t zi = 0; zi < n; ++zi) {
            const int64_t min_z = static_cast<int64_t>(zs[zi]);
            const int64_t max_z = min_z + span;
            uint32_t inside = 0U;
            for (uint32_t k = 0; k < n; ++k) {
                const int64_t x = static_cast<int64_t>(xs[k]);
                const int64_t z = static_cast<int64_t>(zs[k]);
                if (x < min_x || x > max_x || z < min_z || z > max_z) continue;
                if (++inside >= min_required) return true;
            }
        }
    }
    return false;
}

__global__ void filter_multi_exact_kernel_x4(
    uint64_t start_seed,
    uint64_t count,
    uint64_t *out,
    uint32_t *hit_count,
    uint32_t constraint_count,
    const ConstraintDesc * __restrict__ constraints_global) {
    constexpr int N = SEEDS_PER_THREAD;
    const uint64_t tid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const uint64_t base_idx = tid * static_cast<uint64_t>(N);
    if (base_idx >= count) return;

    uint64_t seed48[N];
    bool alive[N];
    #pragma unroll
    for (int i = 0; i < N; ++i) {
        const uint64_t idx = base_idx + static_cast<uint64_t>(i);
        alive[i] = (idx < count);
        seed48[i] = (start_seed + idx) & JAVA_MASK;
    }

    for (uint32_t ci = 0; ci < constraint_count; ++ci) {
        const ConstraintDesc c = constraints_global[ci];
        if (c.is_gate_only != 0U || c.region_count == 0U) continue;

        const uint32_t min_required = c.min_required == 0U ? 1U : c.min_required;
        const uint32_t end = c.region_start + c.region_count;

        // Fast path: min_required==1, no quad-span, no triangular constraints
        // are enforced here. Stop scanning per-lane as soon as a hit lands.
        if (min_required == 1U && c.quad_max_span == 0U) {
            bool found[N];
            #pragma unroll
            for (int i = 0; i < N; ++i) found[i] = false;
            for (uint32_t ri = c.region_start; ri < end; ++ri) {
                const RegionTerm term = k_regions[ri];
                bool hit[N];
                int32_t bx[N], bz[N];
                region_hit_n_with_pos<N>(term, seed48, c.anchor_x, c.anchor_z, c.radius_sq, hit, bx, bz);
                #pragma unroll
                for (int i = 0; i < N; ++i) {
                    found[i] = found[i] || (hit[i] && alive[i]);
                }
                bool any_unfound = false;
                #pragma unroll
                for (int i = 0; i < N; ++i) {
                    if (alive[i] && !found[i]) any_unfound = true;
                }
                if (!any_unfound) break;
            }
            #pragma unroll
            for (int i = 0; i < N; ++i) {
                alive[i] = alive[i] && found[i];
            }
            bool any_alive = false;
            #pragma unroll
            for (int i = 0; i < N; ++i) any_alive = any_alive || alive[i];
            if (!any_alive) return;
            continue;
        }

        // Quad / span path: keep matched coords per lane to evaluate compact
        // group test after region sweep. Falls back to per-lane scalar to keep
        // register pressure manageable.
        if (c.quad_max_span > 0U && min_required > 1U && c.region_count <= MAX_QUAD_CHECK_POINTS) {
            #pragma unroll
            for (int i = 0; i < N; ++i) {
                if (!alive[i]) continue;
                int32_t mx[MAX_QUAD_CHECK_POINTS];
                int32_t mz[MAX_QUAD_CHECK_POINTS];
                uint32_t matched = 0U;
                bool fail = false;
                for (uint32_t ri = c.region_start; ri < end; ++ri) {
                    int32_t bx = 0, bz = 0;
                    if (region_hit_exact(k_regions[ri], seed48[i], c.anchor_x, c.anchor_z, c.radius_sq, &bx, &bz)) {
                        if (matched < MAX_QUAD_CHECK_POINTS) {
                            mx[matched] = bx;
                            mz[matched] = bz;
                        }
                        ++matched;
                    }
                    const uint32_t remaining = end - (ri + 1U);
                    if (matched + remaining < min_required) { fail = true; break; }
                }
                if (fail || matched < min_required ||
                    !has_compact_group_within_span_local(mx, mz, matched, min_required, c.quad_max_span)) {
                    alive[i] = false;
                }
            }
            bool any_alive = false;
            #pragma unroll
            for (int i = 0; i < N; ++i) any_alive = any_alive || alive[i];
            if (!any_alive) return;
            continue;
        }

        // Generic min_required>1, no span: count hits per lane.
        uint32_t matched[N];
        #pragma unroll
        for (int i = 0; i < N; ++i) matched[i] = 0U;
        for (uint32_t ri = c.region_start; ri < end; ++ri) {
            const RegionTerm term = k_regions[ri];
            bool hit[N];
            int32_t bx[N], bz[N];
            region_hit_n_with_pos<N>(term, seed48, c.anchor_x, c.anchor_z, c.radius_sq, hit, bx, bz);
            #pragma unroll
            for (int i = 0; i < N; ++i) {
                if (hit[i]) ++matched[i];
            }
            const uint32_t remaining = end - (ri + 1U);
            bool any_can_pass = false;
            #pragma unroll
            for (int i = 0; i < N; ++i) {
                if (alive[i] && (matched[i] + remaining >= min_required)) any_can_pass = true;
            }
            if (!any_can_pass) {
                #pragma unroll
                for (int i = 0; i < N; ++i) alive[i] = false;
                return;
            }
        }
        #pragma unroll
        for (int i = 0; i < N; ++i) {
            alive[i] = alive[i] && (matched[i] >= min_required);
        }
        bool any_alive = false;
        #pragma unroll
        for (int i = 0; i < N; ++i) any_alive = any_alive || alive[i];
        if (!any_alive) return;
    }

    emit_hits_n<N>(alive, seed48, out, hit_count, count);
}

__global__ void filter_single_kernel(
    uint64_t start_seed,
    uint64_t count,
    uint64_t radius_sq,
    uint32_t gate_div,
    uint32_t gate_salt,
    uint64_t *out,
    uint32_t *hit_count,
    uint32_t region_count) {
    const uint64_t idx = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= count) {
        return;
    }

    const uint64_t seed = start_seed + idx;
    const uint64_t seed48 = seed & JAVA_MASK;
    if (!gate_pass(seed48, gate_div, gate_salt)) {
        return;
    }

    bool found = false;
    for (uint32_t i = 0; i < region_count; ++i) {
        if (region_hit(k_regions[i], seed48, 0, 0, radius_sq)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    const uint32_t pos = atomicAdd(hit_count, 1U);
    if (pos < count) {
        out[pos] = seed48;
    }
}

__global__ void filter_multi_exact_kernel(
    uint64_t start_seed,
    uint64_t count,
    const ConstraintDesc *constraints,
    uint64_t *out,
    uint32_t *hit_count,
    uint32_t constraint_count) {
    const uint64_t idx = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= count) {
        return;
    }

    const uint64_t seed = start_seed + idx;
    const uint64_t seed48 = seed & JAVA_MASK;

    for (uint32_t ci = 0; ci < constraint_count; ++ci) {
        const ConstraintDesc c = constraints[ci];
        // Exact Stage A consumes structure-only constraints. Legacy surrogate
        // entries are skipped so older callers remain compatible while the
        // compiled plan migrates to exact low-48 descriptors.
        if (c.is_gate_only != 0U || c.region_count == 0U) {
            continue;
        }
        const uint32_t min_required = c.min_required == 0U ? 1U : c.min_required;
        uint32_t matched_count = 0U;
        const uint32_t end = c.region_start + c.region_count;
        if (min_required == 1U && c.quad_max_span == 0U) {
            bool matched = false;
            for (uint32_t ri = c.region_start; ri < end; ++ri) {
                if (region_hit_exact(k_regions[ri], seed48, c.anchor_x, c.anchor_z, c.radius_sq)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return;
            }
            continue;
        }
        if (c.quad_max_span > 0U && min_required > 1U && c.region_count <= MAX_QUAD_CHECK_POINTS) {
            int32_t matched_x[MAX_QUAD_CHECK_POINTS];
            int32_t matched_z[MAX_QUAD_CHECK_POINTS];
            for (uint32_t ri = c.region_start; ri < end; ++ri) {
                int32_t bx = 0;
                int32_t bz = 0;
                const bool hit = region_hit_exact(k_regions[ri], seed48, c.anchor_x, c.anchor_z, c.radius_sq, &bx, &bz);
                if (hit) {
                    if (matched_count < MAX_QUAD_CHECK_POINTS) {
                        matched_x[matched_count] = bx;
                        matched_z[matched_count] = bz;
                    }
                }
                matched_count += hit ? 1U : 0U;
                const uint32_t remaining_regions = end - (ri + 1U);
                if (matched_count + remaining_regions < min_required) {
                    return;
                }
            }
            if (matched_count < min_required) {
                return;
            }
            if (!has_compact_group_within_span(
                    matched_x,
                    matched_z,
                    matched_count,
                    min_required,
                    c.quad_max_span)) {
                return;
            }
            continue;
        }
        for (uint32_t ri = c.region_start; ri < end; ++ri) {
            const bool hit = region_hit_exact(k_regions[ri], seed48, c.anchor_x, c.anchor_z, c.radius_sq);
            matched_count += hit ? 1U : 0U;
            if (matched_count >= min_required) {
                break;
            }
            const uint32_t remaining_regions = end - (ri + 1U);
            if (matched_count + remaining_regions < min_required) {
                return;
            }
        }
        if (matched_count < min_required) {
            return;
        }
    }

    const uint32_t pos = atomicAdd(hit_count, 1U);
    if (pos < count) {
        out[pos] = seed48;
    }
}

__global__ void filter_one_min1_kernel(
    uint64_t start_seed,
    uint64_t count,
    uint64_t radius_sq,
    int32_t anchor_x,
    int32_t anchor_z,
    uint32_t region_start,
    uint32_t region_count,
    uint64_t *out,
    uint32_t *hit_count) {
    const uint64_t idx = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= count) {
        return;
    }

    const uint64_t seed48 = (start_seed + idx) & JAVA_MASK;
    const uint32_t end = region_start + region_count;
    for (uint32_t ri = region_start; ri < end; ++ri) {
        if (region_hit_exact(k_regions[ri], seed48, anchor_x, anchor_z, radius_sq)) {
            const uint32_t pos = atomicAdd(hit_count, 1U);
            if (pos < count) {
                out[pos] = seed48;
            }
            return;
        }
    }
}

bool copy_regions_to_constant(const RegionTerm *regions, uint32_t region_count) {
    if (region_count == 0U) {
        return true;
    }
    const size_t bytes = static_cast<size_t>(region_count) * sizeof(RegionTerm);
    return cudaMemcpyToSymbol(k_regions, regions, bytes, 0, cudaMemcpyHostToDevice) == cudaSuccess;
}

// ============================================================================
// Patch 1: double-buffered async Stage A pipeline.
//
// Each GPU device gets an AsyncDeviceState with two AsyncSlotResources entries.
// The host scan driver alternates: submit batch N+1 on slot (cur^1) while
// collecting batch N on slot cur. Submit issues all CUDA work asynchronously
// (memsets, kernels, D2H memcpys via pinned host buffers, event records) and
// returns immediately. Collect blocks on the slot's events, then conditionally
// copies the per-device output into the caller's buffer.
//
// The shared per-device state (regions copy to __constant__, exact constraints
// copy to global) lives on AsyncDeviceState, not the slot. As long as both
// in-flight batches share the same query (the steady state of any scan run),
// they read identical immutable data.
// ============================================================================

struct AsyncSlotResources {
    cudaStream_t stream = nullptr;
    bool stream_ready = false;
    cudaEvent_t kernel_start = nullptr;
    cudaEvent_t kernel_end = nullptr;
    cudaEvent_t transfer_end = nullptr;
    bool events_ready = false;
    uint64_t *d_out = nullptr;
    uint32_t *d_hit = nullptr;
    uint64_t d_out_capacity = 0ULL;
    uint64_t *pinned_out = nullptr;
    uint64_t pinned_capacity = 0ULL;
    uint32_t *pinned_hit = nullptr;
};

struct AsyncDeviceState {
    AsyncSlotResources slots[2];
    uint64_t regions_hash = 0ULL;
    uint32_t regions_count = 0U;
    uint64_t constraints_hash = 0ULL;
    uint32_t constraints_count = 0U;
    ConstraintDesc *d_constraints = nullptr;
    uint32_t constraint_capacity = 0U;
};

std::mutex g_async_state_mu;
std::vector<AsyncDeviceState> g_async_device_states;

AsyncDeviceState &async_state_for_device(int device_index) {
    if (device_index < 0) {
        device_index = 0;
    }
    std::lock_guard<std::mutex> lock(g_async_state_mu);
    if (static_cast<size_t>(device_index) >= g_async_device_states.size()) {
        g_async_device_states.resize(static_cast<size_t>(device_index) + 1U);
    }
    return g_async_device_states[static_cast<size_t>(device_index)];
}

bool ensure_async_slot_resources(AsyncSlotResources &slot, uint64_t needed_out_capacity) {
    if (!slot.stream_ready) {
        if (cudaStreamCreateWithFlags(&slot.stream, cudaStreamNonBlocking) != cudaSuccess) {
            return false;
        }
        slot.stream_ready = true;
    }
    if (!slot.events_ready) {
        if (cudaEventCreateWithFlags(&slot.kernel_start, cudaEventDefault) != cudaSuccess) {
            return false;
        }
        if (cudaEventCreateWithFlags(&slot.kernel_end, cudaEventDefault) != cudaSuccess) {
            return false;
        }
        if (cudaEventCreateWithFlags(&slot.transfer_end, cudaEventDefault) != cudaSuccess) {
            return false;
        }
        slot.events_ready = true;
    }
    if (slot.d_hit == nullptr) {
        if (cudaMalloc(reinterpret_cast<void **>(&slot.d_hit), sizeof(uint32_t)) != cudaSuccess) {
            return false;
        }
    }
    if (slot.d_out == nullptr || slot.d_out_capacity < needed_out_capacity) {
        if (slot.d_out != nullptr) {
            cudaFree(slot.d_out);
            slot.d_out = nullptr;
        }
        uint64_t new_cap = needed_out_capacity;
        if (slot.d_out_capacity > 0ULL) {
            const uint64_t grown = slot.d_out_capacity + slot.d_out_capacity / 2ULL;
            if (grown > new_cap) {
                new_cap = grown;
            }
        }
        new_cap = std::max<uint64_t>(1ULL, new_cap);
        if (cudaMalloc(
                reinterpret_cast<void **>(&slot.d_out),
                static_cast<size_t>(new_cap) * sizeof(uint64_t)) != cudaSuccess) {
            return false;
        }
        slot.d_out_capacity = new_cap;
    }
    if (slot.pinned_out == nullptr || slot.pinned_capacity < needed_out_capacity) {
        if (slot.pinned_out != nullptr) {
            cudaFreeHost(slot.pinned_out);
            slot.pinned_out = nullptr;
        }
        uint64_t new_cap = needed_out_capacity;
        if (slot.pinned_capacity > 0ULL) {
            const uint64_t grown = slot.pinned_capacity + slot.pinned_capacity / 2ULL;
            if (grown > new_cap) {
                new_cap = grown;
            }
        }
        new_cap = std::max<uint64_t>(1ULL, new_cap);
        if (cudaMallocHost(
                reinterpret_cast<void **>(&slot.pinned_out),
                static_cast<size_t>(new_cap) * sizeof(uint64_t)) != cudaSuccess) {
            return false;
        }
        slot.pinned_capacity = new_cap;
    }
    if (slot.pinned_hit == nullptr) {
        if (cudaMallocHost(
                reinterpret_cast<void **>(&slot.pinned_hit),
                sizeof(uint32_t)) != cudaSuccess) {
            return false;
        }
    }
    return true;
}

bool ensure_async_constraint_capacity(AsyncDeviceState &state, uint32_t constraint_count) {
    if (constraint_count == 0U) {
        return true;
    }
    if (state.d_constraints != nullptr && state.constraint_capacity >= constraint_count) {
        return true;
    }
    uint32_t new_cap = constraint_count;
    if (state.constraint_capacity > 0U) {
        const uint32_t grown = state.constraint_capacity + state.constraint_capacity / 2U;
        if (grown > new_cap) {
            new_cap = grown;
        }
    }
    new_cap = std::max<uint32_t>(1U, new_cap);
    ConstraintDesc *new_constraints = nullptr;
    if (cudaMalloc(
            reinterpret_cast<void **>(&new_constraints),
            static_cast<size_t>(new_cap) * sizeof(ConstraintDesc)) != cudaSuccess) {
        return false;
    }
    if (state.d_constraints != nullptr) {
        cudaFree(state.d_constraints);
    }
    state.d_constraints = new_constraints;
    state.constraint_capacity = new_cap;
    state.constraints_hash = 0ULL;
    state.constraints_count = 0U;
    return true;
}

struct PerDeviceJob {
    int device_index = -1;
    uint64_t device_start = 0ULL;
    uint64_t device_count_seeds = 0ULL;
    AsyncSlotResources *slot_res = nullptr;
};

struct AsyncSlotState {
    std::atomic<bool> in_flight{false};
    int status = GPU_FILTER_STATUS_OK;
    uint64_t submitted_count = 0ULL;
    bool nothing_to_filter = false;
    uint64_t passthrough_start = 0ULL;
    uint64_t passthrough_count = 0ULL;
    bool use_single_min1_kernel = false;
    uint32_t exact_constraint_count = 0U;
    std::vector<ConstraintDesc> exact_constraints;
    std::vector<PerDeviceJob> jobs;
};

AsyncSlotState g_async_slots[2];

}  // namespace

extern "C" int gpu_is_available_impl(void) {
    return gpu_device_count_impl() > 0 ? 1 : 0;
}

extern "C" uint64_t gpu_total_mem_impl(void) {
    const int count = gpu_device_count_impl();
    uint64_t total = 0ULL;
    for (int device_index = 0; device_index < count; ++device_index) {
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, device_index) != cudaSuccess) {
            continue;
        }
        total += static_cast<uint64_t>(prop.totalGlobalMem);
    }
    return total;
}

extern "C" void gpu_filter_impl(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    int region_count,
    uint64_t radius_sq,
    uint32_t gate_div,
    uint32_t gate_salt,
    uint64_t *output_buffer,
    uint32_t *hit_count) {
    if (hit_count == nullptr) {
        return;
    }
    *hit_count = 0U;
    if (count == 0ULL || output_buffer == nullptr || regions == nullptr || region_count <= 0) {
        return;
    }
    if (gpu_is_available_impl() != 1) {
        return;
    }
    if (region_count > MAX_CONST_REGIONS) {
        return;
    }
    const uint32_t u_region_count = static_cast<uint32_t>(region_count);
    const uint64_t region_hash = hash_regions(regions, u_region_count);
    const int device_count = gpu_device_count_impl();
    if (device_count <= 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_workspace_mu);
        if (g_workspaces.size() < static_cast<size_t>(device_count)) {
            g_workspaces.resize(static_cast<size_t>(device_count));
        }
    }

    struct WorkerResult {
        uint32_t hit_count = 0U;
        std::vector<uint64_t> hits;
    };

    std::vector<WorkerResult> results(static_cast<size_t>(device_count));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(device_count));
    const uint64_t base_chunk = count / static_cast<uint64_t>(device_count);
    const uint64_t extra = count % static_cast<uint64_t>(device_count);
    uint64_t offset = 0ULL;

    for (int device_index = 0; device_index < device_count; ++device_index) {
        const uint64_t device_count_seeds =
            base_chunk + (static_cast<uint64_t>(device_index) < extra ? 1ULL : 0ULL);
        const uint64_t device_start_seed = start_seed + offset;
        offset += device_count_seeds;
        if (device_count_seeds == 0ULL) {
            continue;
        }

        workers.emplace_back([&, device_index, device_count_seeds, device_start_seed, region_hash, u_region_count, regions]() {
            if (cudaSetDevice(device_index) != cudaSuccess) {
                return;
            }
            DeviceWorkspace &workspace = workspace_for_device(device_index);
            if (workspace.regions_hash != region_hash || workspace.regions_count != u_region_count) {
                if (!copy_regions_to_constant(regions, u_region_count)) {
                    return;
                }
                workspace.regions_hash = region_hash;
                workspace.regions_count = u_region_count;
            }
            if (!ensure_output_capacity(workspace, device_count_seeds)) {
                return;
            }
            cudaStream_t stream = ensure_stream(workspace);
            cudaMemsetAsync(workspace.d_hit, 0, sizeof(uint32_t), stream);

            // Detect whether all regions request the exact path. When they do,
            // we can use the higher-throughput ILP-4 kernel safely. Otherwise
            // we fall back to the legacy single-seed kernel so the
            // use_fast_next_int hint keeps its historical semantics.
            bool all_exact = true;
            for (uint32_t i = 0; i < u_region_count; ++i) {
                if (regions[i].use_fast_next_int != 0U) { all_exact = false; break; }
            }

            uint64_t processed = 0ULL;
            while (processed < device_count_seeds) {
                const uint64_t batch = (device_count_seeds - processed > MAX_SEEDS_PER_LAUNCH)
                    ? MAX_SEEDS_PER_LAUNCH
                    : (device_count_seeds - processed);
                if (all_exact) {
                    const uint64_t threads_needed = (batch + SEEDS_PER_THREAD - 1ULL) / SEEDS_PER_THREAD;
                    const uint32_t blocks = static_cast<uint32_t>((threads_needed + THREADS_PER_BLOCK - 1ULL) / THREADS_PER_BLOCK);
                    filter_single_kernel_x4<<<blocks, THREADS_PER_BLOCK, 0, stream>>>(
                        device_start_seed + processed,
                        batch,
                        radius_sq,
                        gate_div,
                        gate_salt,
                        workspace.d_out,
                        workspace.d_hit,
                        u_region_count);
                } else {
                    const uint32_t blocks = static_cast<uint32_t>((batch + THREADS_PER_BLOCK - 1ULL) / THREADS_PER_BLOCK);
                    filter_single_kernel<<<blocks, THREADS_PER_BLOCK, 0, stream>>>(
                        device_start_seed + processed,
                        batch,
                        radius_sq,
                        gate_div,
                        gate_salt,
                        workspace.d_out,
                        workspace.d_hit,
                        u_region_count);
                }
                if (cudaGetLastError() != cudaSuccess) {
                    return;
                }
                processed += batch;
            }

            uint32_t host_hits = 0U;
            cudaStreamSynchronize(stream);
            cudaMemcpy(&host_hits, workspace.d_hit, sizeof(uint32_t), cudaMemcpyDeviceToHost);
            if (host_hits > device_count_seeds) {
                host_hits = static_cast<uint32_t>(device_count_seeds);
            }
            WorkerResult &result = results[static_cast<size_t>(device_index)];
            result.hit_count = host_hits;
            if (host_hits > 0U) {
                result.hits.resize(host_hits);
                cudaMemcpy(
                    result.hits.data(),
                    workspace.d_out,
                    static_cast<size_t>(host_hits) * sizeof(uint64_t),
                    cudaMemcpyDeviceToHost
                );
            }
        });
    }

    for (std::thread &worker : workers) {
        worker.join();
    }

    uint32_t merged_hits = 0U;
    for (const WorkerResult &result : results) {
        if (result.hit_count == 0U) {
            continue;
        }
        std::copy(result.hits.begin(), result.hits.end(), output_buffer + merged_hits);
        merged_hits += result.hit_count;
    }
    *hit_count = merged_hits;
}

extern "C" int gpu_filter_multi_checked_impl(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count) {
    if (hit_count == nullptr) {
        return GPU_FILTER_STATUS_INVALID_ARG;
    }
    *hit_count = 0U;
    if (count == 0ULL || output_buffer == nullptr || constraints == nullptr || constraint_count == 0U) {
        return GPU_FILTER_STATUS_INVALID_ARG;
    }
    if (gpu_is_available_impl() != 1) {
        return GPU_FILTER_STATUS_NO_DEVICE;
    }
    if (region_count > static_cast<uint32_t>(MAX_CONST_REGIONS)) {
        return GPU_FILTER_STATUS_REGION_LIMIT;
    }
    const uint64_t region_hash = hash_regions(regions, region_count);
    std::vector<ConstraintDesc> exact_constraints;
    exact_constraints.reserve(constraint_count);
    for (uint32_t i = 0; i < constraint_count; ++i) {
        const ConstraintDesc c = constraints[i];
        if (c.is_gate_only != 0U || c.region_count == 0U) {
            continue;
        }
        exact_constraints.push_back(c);
    }
    if (exact_constraints.empty()) {
        for (uint64_t i = 0; i < count; ++i) {
            output_buffer[i] = (start_seed + i) & JAVA_MASK;
        }
        *hit_count = static_cast<uint32_t>(count);
        return GPU_FILTER_STATUS_OK;
    }

    const uint32_t exact_constraint_count = static_cast<uint32_t>(exact_constraints.size());
    const uint64_t constraints_hash = hash_constraints(exact_constraints.data(), exact_constraint_count);
    const bool use_single_min1_kernel =
        exact_constraint_count == 1U &&
        (exact_constraints[0].min_required == 0U || exact_constraints[0].min_required == 1U) &&
        exact_constraints[0].quad_max_span == 0U;
    const int device_count = gpu_device_count_impl();
    if (device_count <= 0) {
        return GPU_FILTER_STATUS_NO_DEVICE;
    }
    {
        std::lock_guard<std::mutex> lock(g_workspace_mu);
        if (g_workspaces.size() < static_cast<size_t>(device_count)) {
            g_workspaces.resize(static_cast<size_t>(device_count));
        }
    }

    struct WorkerResult {
        uint32_t hit_count = 0U;
        std::vector<uint64_t> hits;
    };

    std::vector<WorkerResult> results(static_cast<size_t>(device_count));
    std::vector<int> worker_status(static_cast<size_t>(device_count), GPU_FILTER_STATUS_OK);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(device_count));
    const uint64_t base_chunk = count / static_cast<uint64_t>(device_count);
    const uint64_t extra = count % static_cast<uint64_t>(device_count);
    uint64_t offset = 0ULL;

    for (int device_index = 0; device_index < device_count; ++device_index) {
        const uint64_t device_count_seeds =
            base_chunk + (static_cast<uint64_t>(device_index) < extra ? 1ULL : 0ULL);
        const uint64_t device_start_seed = start_seed + offset;
        offset += device_count_seeds;
        if (device_count_seeds == 0ULL) {
            continue;
        }

        workers.emplace_back([&, device_index, device_count_seeds, device_start_seed, region_hash, constraints_hash, region_count, exact_constraint_count, regions]() {
            if (cudaSetDevice(device_index) != cudaSuccess) {
                worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_DEVICE_SETUP;
                return;
            }
            DeviceWorkspace &workspace = workspace_for_device(device_index);
            if (workspace.regions_hash != region_hash || workspace.regions_count != region_count) {
                if (!copy_regions_to_constant(regions, region_count)) {
                    worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_COPY;
                    return;
                }
                workspace.regions_hash = region_hash;
                workspace.regions_count = region_count;
            }
            if (!ensure_output_capacity(workspace, device_count_seeds)) {
                worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_ALLOC;
                return;
            }
            cudaStream_t stream = ensure_stream(workspace);
            if (!use_single_min1_kernel) {
                if (!ensure_constraint_capacity(workspace, exact_constraint_count)) {
                    worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_ALLOC;
                    return;
                }
                if (workspace.constraints_hash != constraints_hash || workspace.constraints_count != exact_constraint_count) {
                    if (cudaMemcpyAsync(
                            workspace.d_constraints,
                            exact_constraints.data(),
                            static_cast<size_t>(exact_constraint_count) * sizeof(ConstraintDesc),
                            cudaMemcpyHostToDevice,
                            stream) != cudaSuccess) {
                        worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_COPY;
                        return;
                    }
                    workspace.constraints_hash = constraints_hash;
                    workspace.constraints_count = exact_constraint_count;
                }
            }
            if (cudaMemsetAsync(workspace.d_hit, 0, sizeof(uint32_t), stream) != cudaSuccess) {
                worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_COPY;
                return;
            }

            uint64_t processed = 0ULL;
            while (processed < device_count_seeds) {
                const uint64_t batch = (device_count_seeds - processed > MAX_SEEDS_PER_LAUNCH)
                    ? MAX_SEEDS_PER_LAUNCH
                    : (device_count_seeds - processed);
                const uint64_t threads_needed = (batch + SEEDS_PER_THREAD - 1ULL) / SEEDS_PER_THREAD;
                const uint32_t blocks = static_cast<uint32_t>((threads_needed + THREADS_PER_BLOCK - 1ULL) / THREADS_PER_BLOCK);
                if (use_single_min1_kernel) {
                    const ConstraintDesc &c = exact_constraints[0];
                    filter_one_min1_kernel_x4<<<blocks, THREADS_PER_BLOCK, 0, stream>>>(
                        device_start_seed + processed,
                        batch,
                        c.radius_sq,
                        c.anchor_x,
                        c.anchor_z,
                        c.region_start,
                        c.region_count,
                        workspace.d_out,
                        workspace.d_hit);
                } else {
                    filter_multi_exact_kernel_x4<<<blocks, THREADS_PER_BLOCK, 0, stream>>>(
                        device_start_seed + processed,
                        batch,
                        workspace.d_out,
                        workspace.d_hit,
                        exact_constraint_count,
                        workspace.d_constraints);
                }
                if (cudaGetLastError() != cudaSuccess) {
                    worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_LAUNCH;
                    return;
                }
                processed += batch;
            }

            uint32_t host_hits = 0U;
            cudaStreamSynchronize(stream);
            if (cudaMemcpy(&host_hits, workspace.d_hit, sizeof(uint32_t), cudaMemcpyDeviceToHost) != cudaSuccess) {
                worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_COPY;
                return;
            }
            if (host_hits > device_count_seeds) {
                host_hits = static_cast<uint32_t>(device_count_seeds);
            }
            WorkerResult &result = results[static_cast<size_t>(device_index)];
            result.hit_count = host_hits;
            if (host_hits > 0U) {
                result.hits.resize(host_hits);
                if (cudaMemcpy(
                        result.hits.data(),
                        workspace.d_out,
                        static_cast<size_t>(host_hits) * sizeof(uint64_t),
                        cudaMemcpyDeviceToHost) != cudaSuccess) {
                    worker_status[static_cast<size_t>(device_index)] = GPU_FILTER_STATUS_COPY;
                    result.hits.clear();
                    result.hit_count = 0U;
                    return;
                }
            }
        });
    }

    for (std::thread &worker : workers) {
        worker.join();
    }

    for (int status : worker_status) {
        if (status != GPU_FILTER_STATUS_OK) {
            *hit_count = 0U;
            return status;
        }
    }

    uint32_t merged_hits = 0U;
    for (const WorkerResult &result : results) {
        if (result.hit_count == 0U) {
            continue;
        }
        std::copy(result.hits.begin(), result.hits.end(), output_buffer + merged_hits);
        merged_hits += result.hit_count;
    }
    *hit_count = merged_hits;
    return GPU_FILTER_STATUS_OK;
}

extern "C" void gpu_filter_multi_impl(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count) {
    (void)gpu_filter_multi_checked_impl(
        start_seed,
        count,
        regions,
        region_count,
        constraints,
        constraint_count,
        output_buffer,
        hit_count
    );
}

extern "C" int gpu_filter_double_buffer_available_impl(void) {
    return 1;
}

extern "C" int gpu_filter_multi_submit_impl(
    uint32_t slot_id,
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count) {
    if (slot_id >= 2U) {
        return GPU_FILTER_STATUS_INVALID_ARG;
    }
    AsyncSlotState &slot = g_async_slots[slot_id];
    bool expected_idle = false;
    if (!slot.in_flight.compare_exchange_strong(expected_idle, true)) {
        return GPU_FILTER_STATUS_INVALID_ARG;
    }

    slot.status = GPU_FILTER_STATUS_OK;
    slot.submitted_count = count;
    slot.nothing_to_filter = false;
    slot.passthrough_start = start_seed;
    slot.passthrough_count = 0ULL;
    slot.use_single_min1_kernel = false;
    slot.exact_constraint_count = 0U;
    slot.exact_constraints.clear();
    slot.jobs.clear();

    if (count == 0ULL) {
        slot.nothing_to_filter = true;
        return GPU_FILTER_STATUS_OK;
    }

    if (constraints == nullptr || constraint_count == 0U) {
        slot.in_flight.store(false);
        return GPU_FILTER_STATUS_INVALID_ARG;
    }
    if (gpu_is_available_impl() != 1) {
        slot.in_flight.store(false);
        return GPU_FILTER_STATUS_NO_DEVICE;
    }
    if (region_count > static_cast<uint32_t>(MAX_CONST_REGIONS)) {
        slot.in_flight.store(false);
        return GPU_FILTER_STATUS_REGION_LIMIT;
    }

    slot.exact_constraints.reserve(constraint_count);
    for (uint32_t i = 0; i < constraint_count; ++i) {
        const ConstraintDesc c = constraints[i];
        if (c.is_gate_only != 0U || c.region_count == 0U) {
            continue;
        }
        slot.exact_constraints.push_back(c);
    }
    if (slot.exact_constraints.empty()) {
        slot.nothing_to_filter = true;
        slot.passthrough_count = count;
        return GPU_FILTER_STATUS_OK;
    }

    slot.exact_constraint_count = static_cast<uint32_t>(slot.exact_constraints.size());
    const uint64_t region_hash = hash_regions(regions, region_count);
    const uint64_t constraints_hash =
        hash_constraints(slot.exact_constraints.data(), slot.exact_constraint_count);
    slot.use_single_min1_kernel =
        slot.exact_constraint_count == 1U &&
        (slot.exact_constraints[0].min_required == 0U || slot.exact_constraints[0].min_required == 1U) &&
        slot.exact_constraints[0].quad_max_span == 0U;

    const int device_count = gpu_device_count_impl();
    if (device_count <= 0) {
        slot.in_flight.store(false);
        return GPU_FILTER_STATUS_NO_DEVICE;
    }

    const uint64_t base_chunk = count / static_cast<uint64_t>(device_count);
    const uint64_t extra = count % static_cast<uint64_t>(device_count);
    uint64_t offset = 0ULL;
    slot.jobs.reserve(static_cast<size_t>(device_count));

    for (int di = 0; di < device_count; ++di) {
        const uint64_t dc =
            base_chunk + (static_cast<uint64_t>(di) < extra ? 1ULL : 0ULL);
        const uint64_t ds = start_seed + offset;
        offset += dc;
        if (dc == 0ULL) {
            continue;
        }

        if (cudaSetDevice(di) != cudaSuccess) {
            slot.status = GPU_FILTER_STATUS_DEVICE_SETUP;
            slot.in_flight.store(false);
            return GPU_FILTER_STATUS_DEVICE_SETUP;
        }

        AsyncDeviceState &state = async_state_for_device(di);
        AsyncSlotResources &res = state.slots[slot_id];

        if (state.regions_hash != region_hash || state.regions_count != region_count) {
            if (!copy_regions_to_constant(regions, region_count)) {
                slot.status = GPU_FILTER_STATUS_COPY;
                slot.in_flight.store(false);
                return GPU_FILTER_STATUS_COPY;
            }
            state.regions_hash = region_hash;
            state.regions_count = region_count;
        }

        if (!ensure_async_slot_resources(res, dc)) {
            slot.status = GPU_FILTER_STATUS_ALLOC;
            slot.in_flight.store(false);
            return GPU_FILTER_STATUS_ALLOC;
        }

        if (!slot.use_single_min1_kernel) {
            if (!ensure_async_constraint_capacity(state, slot.exact_constraint_count)) {
                slot.status = GPU_FILTER_STATUS_ALLOC;
                slot.in_flight.store(false);
                return GPU_FILTER_STATUS_ALLOC;
            }
            if (state.constraints_hash != constraints_hash ||
                state.constraints_count != slot.exact_constraint_count) {
                if (cudaMemcpyAsync(
                        state.d_constraints,
                        slot.exact_constraints.data(),
                        static_cast<size_t>(slot.exact_constraint_count) * sizeof(ConstraintDesc),
                        cudaMemcpyHostToDevice,
                        res.stream) != cudaSuccess) {
                    slot.status = GPU_FILTER_STATUS_COPY;
                    slot.in_flight.store(false);
                    return GPU_FILTER_STATUS_COPY;
                }
                state.constraints_hash = constraints_hash;
                state.constraints_count = slot.exact_constraint_count;
            }
        }

        if (cudaEventRecord(res.kernel_start, res.stream) != cudaSuccess) {
            slot.status = GPU_FILTER_STATUS_LAUNCH;
            slot.in_flight.store(false);
            return GPU_FILTER_STATUS_LAUNCH;
        }
        if (cudaMemsetAsync(res.d_hit, 0, sizeof(uint32_t), res.stream) != cudaSuccess) {
            slot.status = GPU_FILTER_STATUS_COPY;
            slot.in_flight.store(false);
            return GPU_FILTER_STATUS_COPY;
        }

        uint64_t processed = 0ULL;
        while (processed < dc) {
            const uint64_t batch = (dc - processed > MAX_SEEDS_PER_LAUNCH)
                ? MAX_SEEDS_PER_LAUNCH
                : (dc - processed);
            const uint64_t threads_needed =
                (batch + SEEDS_PER_THREAD - 1ULL) / SEEDS_PER_THREAD;
            const uint32_t blocks = static_cast<uint32_t>(
                (threads_needed + THREADS_PER_BLOCK - 1ULL) / THREADS_PER_BLOCK);
            if (slot.use_single_min1_kernel) {
                const ConstraintDesc &c = slot.exact_constraints[0];
                filter_one_min1_kernel_x4<<<blocks, THREADS_PER_BLOCK, 0, res.stream>>>(
                    ds + processed,
                    batch,
                    c.radius_sq,
                    c.anchor_x,
                    c.anchor_z,
                    c.region_start,
                    c.region_count,
                    res.d_out,
                    res.d_hit);
            } else {
                filter_multi_exact_kernel_x4<<<blocks, THREADS_PER_BLOCK, 0, res.stream>>>(
                    ds + processed,
                    batch,
                    res.d_out,
                    res.d_hit,
                    slot.exact_constraint_count,
                    state.d_constraints);
            }
            if (cudaGetLastError() != cudaSuccess) {
                slot.status = GPU_FILTER_STATUS_LAUNCH;
                slot.in_flight.store(false);
                return GPU_FILTER_STATUS_LAUNCH;
            }
            processed += batch;
        }

        if (cudaMemcpyAsync(
                res.pinned_hit,
                res.d_hit,
                sizeof(uint32_t),
                cudaMemcpyDeviceToHost,
                res.stream) != cudaSuccess) {
            slot.status = GPU_FILTER_STATUS_COPY;
            slot.in_flight.store(false);
            return GPU_FILTER_STATUS_COPY;
        }
        if (cudaEventRecord(res.kernel_end, res.stream) != cudaSuccess) {
            slot.status = GPU_FILTER_STATUS_LAUNCH;
            slot.in_flight.store(false);
            return GPU_FILTER_STATUS_LAUNCH;
        }

        PerDeviceJob job;
        job.device_index = di;
        job.device_start = ds;
        job.device_count_seeds = dc;
        job.slot_res = &res;
        slot.jobs.push_back(job);
    }

    return GPU_FILTER_STATUS_OK;
}

extern "C" int gpu_filter_multi_collect_impl(
    uint32_t slot_id,
    uint64_t *out_buffer,
    uint64_t out_capacity,
    uint32_t *out_hit_count,
    double *out_kernel_seconds,
    double *out_transfer_seconds) {
    if (slot_id >= 2U || out_hit_count == nullptr) {
        return GPU_FILTER_STATUS_INVALID_ARG;
    }
    *out_hit_count = 0U;
    if (out_kernel_seconds != nullptr) {
        *out_kernel_seconds = 0.0;
    }
    if (out_transfer_seconds != nullptr) {
        *out_transfer_seconds = 0.0;
    }

    AsyncSlotState &slot = g_async_slots[slot_id];
    if (!slot.in_flight.load()) {
        return GPU_FILTER_STATUS_OK;
    }

    if (slot.status != GPU_FILTER_STATUS_OK) {
        const int rc = slot.status;
        slot.jobs.clear();
        slot.in_flight.store(false);
        return rc;
    }

    if (slot.nothing_to_filter) {
        const uint64_t count = slot.passthrough_count;
        const uint64_t emit = std::min<uint64_t>(count, out_capacity);
        if (emit > 0ULL && out_buffer != nullptr) {
            for (uint64_t i = 0; i < emit; ++i) {
                out_buffer[i] = (slot.passthrough_start + i) & JAVA_MASK;
            }
        }
        *out_hit_count = static_cast<uint32_t>(emit);
        slot.jobs.clear();
        slot.in_flight.store(false);
        return GPU_FILTER_STATUS_OK;
    }

    double max_kernel_sec = 0.0;
    double max_transfer_sec = 0.0;
    uint32_t merged = 0U;
    int rc = GPU_FILTER_STATUS_OK;

    for (PerDeviceJob &job : slot.jobs) {
        if (cudaSetDevice(job.device_index) != cudaSuccess) {
            rc = GPU_FILTER_STATUS_DEVICE_SETUP;
            break;
        }
        AsyncSlotResources &res = *job.slot_res;

        if (cudaEventSynchronize(res.kernel_end) != cudaSuccess) {
            rc = GPU_FILTER_STATUS_LAUNCH;
            break;
        }

        uint32_t n = (res.pinned_hit != nullptr) ? *res.pinned_hit : 0U;
        if (static_cast<uint64_t>(n) > job.device_count_seeds) {
            n = static_cast<uint32_t>(job.device_count_seeds);
        }

        if (n > 0U) {
            if (cudaMemcpyAsync(
                    res.pinned_out,
                    res.d_out,
                    static_cast<size_t>(n) * sizeof(uint64_t),
                    cudaMemcpyDeviceToHost,
                    res.stream) != cudaSuccess) {
                rc = GPU_FILTER_STATUS_COPY;
                break;
            }
        }
        if (cudaEventRecord(res.transfer_end, res.stream) != cudaSuccess) {
            rc = GPU_FILTER_STATUS_LAUNCH;
            break;
        }
        if (cudaEventSynchronize(res.transfer_end) != cudaSuccess) {
            rc = GPU_FILTER_STATUS_COPY;
            break;
        }

        float k_ms = 0.0f;
        float t_ms = 0.0f;
        cudaEventElapsedTime(&k_ms, res.kernel_start, res.kernel_end);
        cudaEventElapsedTime(&t_ms, res.kernel_end, res.transfer_end);
        const double k_sec = static_cast<double>(k_ms) / 1000.0;
        const double t_sec = static_cast<double>(t_ms) / 1000.0;
        if (k_sec > max_kernel_sec) {
            max_kernel_sec = k_sec;
        }
        if (t_sec > max_transfer_sec) {
            max_transfer_sec = t_sec;
        }

        if (n > 0U && out_buffer != nullptr && merged < out_capacity) {
            const uint64_t can_copy =
                std::min<uint64_t>(static_cast<uint64_t>(n), out_capacity - merged);
            std::memcpy(
                out_buffer + merged,
                res.pinned_out,
                static_cast<size_t>(can_copy) * sizeof(uint64_t));
            merged += static_cast<uint32_t>(can_copy);
        }
    }

    *out_hit_count = merged;
    if (out_kernel_seconds != nullptr) {
        *out_kernel_seconds = max_kernel_sec;
    }
    if (out_transfer_seconds != nullptr) {
        *out_transfer_seconds = max_transfer_sec;
    }
    slot.jobs.clear();
    slot.in_flight.store(false);
    return rc;
}
