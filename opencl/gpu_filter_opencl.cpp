// OpenCL implementation of the gpu_filter ABI defined in cuda/gpu_filter.h.
//
// This module mirrors the CUDA backend's exported C ABI so scanner_native.cpp
// can load either DLL/.so/.dylib interchangeably. The OpenCL runtime itself is
// loaded dynamically (LoadLibrary / dlopen) so this library has no link-time
// dependency on any OpenCL SDK. If no OpenCL ICD is installed,
// gpu_is_available() returns 0 cleanly and the scanner reports a load error.
//
// Kernel parity with cuda/gpu_filter.cu:
//   - Same exact next_int rejection logic
//   - Same region_hit math (linear and triangular spread)
//   - Same multi-constraint loop including quad-span detection and min_required
//
// Performance notes vs CUDA:
//   - No 4-way ILP micro-tiling — OpenCL drivers typically auto-vectorize
//   - No double-buffer async API (sync only); the scanner gracefully falls
//     back to the sync path when gpu_filter_double_buffer_available returns 0
//   - Uses one device only (the first GPU enumerated). Multi-GPU is a future
//     enhancement.

#include "../cuda/gpu_filter.h"

#include <stdint.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ============================================================================
// Vendored OpenCL declarations.
//
// We intentionally avoid including <CL/cl.h> so this library builds without
// requiring an OpenCL SDK on the build machine. Only the subset of types,
// constants and function pointer typedefs that the implementation uses is
// declared here. These declarations follow the Khronos OpenCL 1.2 ABI.
// ============================================================================

extern "C" {

typedef int32_t          cl_int;
typedef uint32_t         cl_uint;
typedef int64_t          cl_long;
typedef uint64_t         cl_ulong;
typedef cl_uint          cl_bool;
typedef cl_uint          cl_device_type;
typedef cl_uint          cl_device_info;
typedef cl_uint          cl_platform_info;
typedef cl_uint          cl_program_info;
typedef cl_uint          cl_program_build_info;
typedef cl_ulong         cl_mem_flags;
typedef cl_ulong         cl_command_queue_properties;
typedef intptr_t         cl_context_properties;
typedef intptr_t         cl_queue_properties;

typedef struct _cl_platform_id      *cl_platform_id;
typedef struct _cl_device_id        *cl_device_id;
typedef struct _cl_context          *cl_context;
typedef struct _cl_command_queue    *cl_command_queue;
typedef struct _cl_program          *cl_program;
typedef struct _cl_kernel           *cl_kernel;
typedef struct _cl_mem              *cl_mem;
typedef struct _cl_event            *cl_event;

#define CL_SUCCESS                                  0
#define CL_DEVICE_NOT_FOUND                         -1
#define CL_BUILD_PROGRAM_FAILURE                    -11

#define CL_TRUE                                     1
#define CL_FALSE                                    0

#define CL_DEVICE_TYPE_DEFAULT                      (1u << 0)
#define CL_DEVICE_TYPE_CPU                          (1u << 1)
#define CL_DEVICE_TYPE_GPU                          (1u << 2)
#define CL_DEVICE_TYPE_ACCELERATOR                  (1u << 3)
#define CL_DEVICE_TYPE_ALL                          0xFFFFFFFFu

#define CL_PLATFORM_NAME                            0x0902
#define CL_DEVICE_NAME                              0x102B
#define CL_DEVICE_VENDOR                            0x102C
#define CL_DEVICE_VERSION                           0x102F
#define CL_DEVICE_GLOBAL_MEM_SIZE                   0x101F
#define CL_DEVICE_MAX_WORK_GROUP_SIZE               0x1004
#define CL_DEVICE_MAX_COMPUTE_UNITS                 0x1002
#define CL_DEVICE_TYPE                              0x1000

#define CL_MEM_READ_WRITE                           (1u << 0)
#define CL_MEM_WRITE_ONLY                           (1u << 1)
#define CL_MEM_READ_ONLY                            (1u << 2)
#define CL_MEM_USE_HOST_PTR                         (1u << 3)
#define CL_MEM_ALLOC_HOST_PTR                       (1u << 4)
#define CL_MEM_COPY_HOST_PTR                        (1u << 5)

#define CL_PROGRAM_BUILD_LOG                        0x1183

typedef cl_int      (*PFN_clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
typedef cl_int      (*PFN_clGetPlatformInfo)(cl_platform_id, cl_platform_info, size_t, void *, size_t *);
typedef cl_int      (*PFN_clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
typedef cl_int      (*PFN_clGetDeviceInfo)(cl_device_id, cl_device_info, size_t, void *, size_t *);
typedef cl_context  (*PFN_clCreateContext)(const cl_context_properties *, cl_uint, const cl_device_id *,
                                           void (*)(const char *, const void *, size_t, void *),
                                           void *, cl_int *);
typedef cl_command_queue (*PFN_clCreateCommandQueueWithProperties)(cl_context, cl_device_id,
                                                                   const cl_queue_properties *, cl_int *);
typedef cl_command_queue (*PFN_clCreateCommandQueue)(cl_context, cl_device_id,
                                                    cl_command_queue_properties, cl_int *);
typedef cl_program  (*PFN_clCreateProgramWithSource)(cl_context, cl_uint, const char **,
                                                    const size_t *, cl_int *);
typedef cl_int      (*PFN_clBuildProgram)(cl_program, cl_uint, const cl_device_id *, const char *,
                                          void (*)(cl_program, void *), void *);
typedef cl_int      (*PFN_clGetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info,
                                                 size_t, void *, size_t *);
typedef cl_kernel   (*PFN_clCreateKernel)(cl_program, const char *, cl_int *);
typedef cl_mem      (*PFN_clCreateBuffer)(cl_context, cl_mem_flags, size_t, void *, cl_int *);
typedef cl_int      (*PFN_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                                const void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int      (*PFN_clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                               void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int      (*PFN_clEnqueueFillBuffer)(cl_command_queue, cl_mem, const void *, size_t,
                                               size_t, size_t, cl_uint, const cl_event *, cl_event *);
typedef cl_int      (*PFN_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint,
                                                  const size_t *, const size_t *, const size_t *,
                                                  cl_uint, const cl_event *, cl_event *);
typedef cl_int      (*PFN_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
typedef cl_int      (*PFN_clFinish)(cl_command_queue);
typedef cl_int      (*PFN_clFlush)(cl_command_queue);
typedef cl_int      (*PFN_clReleaseMemObject)(cl_mem);
typedef cl_int      (*PFN_clReleaseKernel)(cl_kernel);
typedef cl_int      (*PFN_clReleaseProgram)(cl_program);
typedef cl_int      (*PFN_clReleaseCommandQueue)(cl_command_queue);
typedef cl_int      (*PFN_clReleaseContext)(cl_context);
typedef cl_int      (*PFN_clReleaseDevice)(cl_device_id);

}  // extern "C"

// ============================================================================
// Runtime loader for the system OpenCL ICD.
// ============================================================================

namespace {

struct OpenCLApi {
    PFN_clGetPlatformIDs                  GetPlatformIDs                  = nullptr;
    PFN_clGetPlatformInfo                 GetPlatformInfo                 = nullptr;
    PFN_clGetDeviceIDs                    GetDeviceIDs                    = nullptr;
    PFN_clGetDeviceInfo                   GetDeviceInfo                   = nullptr;
    PFN_clCreateContext                   CreateContext                   = nullptr;
    PFN_clCreateCommandQueueWithProperties CreateCommandQueueWithProperties = nullptr;
    PFN_clCreateCommandQueue              CreateCommandQueue              = nullptr;  // OpenCL 1.x fallback
    PFN_clCreateProgramWithSource         CreateProgramWithSource         = nullptr;
    PFN_clBuildProgram                    BuildProgram                    = nullptr;
    PFN_clGetProgramBuildInfo             GetProgramBuildInfo             = nullptr;
    PFN_clCreateKernel                    CreateKernel                    = nullptr;
    PFN_clCreateBuffer                    CreateBuffer                    = nullptr;
    PFN_clEnqueueWriteBuffer              EnqueueWriteBuffer              = nullptr;
    PFN_clEnqueueReadBuffer               EnqueueReadBuffer               = nullptr;
    PFN_clEnqueueFillBuffer               EnqueueFillBuffer               = nullptr;
    PFN_clEnqueueNDRangeKernel            EnqueueNDRangeKernel            = nullptr;
    PFN_clSetKernelArg                    SetKernelArg                    = nullptr;
    PFN_clFinish                          Finish                          = nullptr;
    PFN_clFlush                           Flush                           = nullptr;
    PFN_clReleaseMemObject                ReleaseMemObject                = nullptr;
    PFN_clReleaseKernel                   ReleaseKernel                   = nullptr;
    PFN_clReleaseProgram                  ReleaseProgram                  = nullptr;
    PFN_clReleaseCommandQueue             ReleaseCommandQueue             = nullptr;
    PFN_clReleaseContext                  ReleaseContext                  = nullptr;
    PFN_clReleaseDevice                   ReleaseDevice                   = nullptr;
    bool ready = false;
};

#if defined(_WIN32)
using DllHandle = HMODULE;
static DllHandle dll_open(const char *name) { return LoadLibraryA(name); }
static void *    dll_sym(DllHandle h, const char *name) {
    return reinterpret_cast<void *>(GetProcAddress(h, name));
}
#else
using DllHandle = void *;
static DllHandle dll_open(const char *name) { return dlopen(name, RTLD_LAZY | RTLD_LOCAL); }
static void *    dll_sym(DllHandle h, const char *name) { return dlsym(h, name); }
#endif

static const char *const kOpenCLCandidates[] = {
#if defined(_WIN32)
    "OpenCL.dll",
#elif defined(__APPLE__)
    // macOS deprecated OpenCL but ships it inside the framework.
    "/System/Library/Frameworks/OpenCL.framework/OpenCL",
    "libOpenCL.dylib",
#else
    "libOpenCL.so.1",
    "libOpenCL.so",
#endif
};

OpenCLApi &cl_api() {
    static OpenCLApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        DllHandle handle = nullptr;
        const char *env = std::getenv("OPENCL_LIBRARY");
        if (env != nullptr && *env != '\0') {
            handle = dll_open(env);
        }
        for (size_t i = 0; handle == nullptr && i < sizeof(kOpenCLCandidates) / sizeof(kOpenCLCandidates[0]); ++i) {
            handle = dll_open(kOpenCLCandidates[i]);
        }
        if (handle == nullptr) {
            return;
        }
        #define RESOLVE_REQ(field, name) \
            api.field = reinterpret_cast<decltype(api.field)>(dll_sym(handle, name)); \
            if (api.field == nullptr) { return; }
        #define RESOLVE_OPT(field, name) \
            api.field = reinterpret_cast<decltype(api.field)>(dll_sym(handle, name));

        RESOLVE_REQ(GetPlatformIDs,           "clGetPlatformIDs");
        RESOLVE_OPT(GetPlatformInfo,          "clGetPlatformInfo");
        RESOLVE_REQ(GetDeviceIDs,             "clGetDeviceIDs");
        RESOLVE_REQ(GetDeviceInfo,            "clGetDeviceInfo");
        RESOLVE_REQ(CreateContext,            "clCreateContext");
        RESOLVE_OPT(CreateCommandQueueWithProperties, "clCreateCommandQueueWithProperties");
        RESOLVE_OPT(CreateCommandQueue,       "clCreateCommandQueue");
        RESOLVE_REQ(CreateProgramWithSource,  "clCreateProgramWithSource");
        RESOLVE_REQ(BuildProgram,             "clBuildProgram");
        RESOLVE_REQ(GetProgramBuildInfo,      "clGetProgramBuildInfo");
        RESOLVE_REQ(CreateKernel,             "clCreateKernel");
        RESOLVE_REQ(CreateBuffer,             "clCreateBuffer");
        RESOLVE_REQ(EnqueueWriteBuffer,       "clEnqueueWriteBuffer");
        RESOLVE_REQ(EnqueueReadBuffer,        "clEnqueueReadBuffer");
        RESOLVE_OPT(EnqueueFillBuffer,        "clEnqueueFillBuffer");
        RESOLVE_REQ(EnqueueNDRangeKernel,     "clEnqueueNDRangeKernel");
        RESOLVE_REQ(SetKernelArg,             "clSetKernelArg");
        RESOLVE_REQ(Finish,                   "clFinish");
        RESOLVE_OPT(Flush,                    "clFlush");
        RESOLVE_REQ(ReleaseMemObject,         "clReleaseMemObject");
        RESOLVE_REQ(ReleaseKernel,            "clReleaseKernel");
        RESOLVE_REQ(ReleaseProgram,           "clReleaseProgram");
        RESOLVE_REQ(ReleaseCommandQueue,      "clReleaseCommandQueue");
        RESOLVE_REQ(ReleaseContext,           "clReleaseContext");
        RESOLVE_OPT(ReleaseDevice,            "clReleaseDevice");
        if (api.CreateCommandQueueWithProperties == nullptr && api.CreateCommandQueue == nullptr) {
            return;
        }
        api.ready = true;
        #undef RESOLVE_REQ
        #undef RESOLVE_OPT
    });
    return api;
}

// ============================================================================
// OpenCL kernel source.
//
// Mirrors the scalar paths of cuda/gpu_filter.cu. Two kernels are exposed:
//   - filter_one_min1: single-constraint, min_required<=1, no quad-span.
//   - filter_multi_exact: general path supporting multiple constraints with
//     min_required, quad_max_span, triangular spread, etc.
//
// Struct layouts match the host-side packed structs in gpu_filter.h. We pin
// alignment with the standard OpenCL rules (ulong is 8-byte aligned). The
// host code asserts sizeof(RegionTerm)==32 and sizeof(ConstraintDesc)==48 so
// any padding drift between host and device shows up at load time.
// ============================================================================

static const char *const kKernelSource = R"OCL(
#define JAVA_MULT  25214903917UL
#define JAVA_ADD   11UL
#define JAVA_MASK  ((1UL << 48) - 1UL)
#define INT32_SIGN 0x80000000U
#define CHUNK_SIZE 16
#define SPREAD_LINEAR     0u
#define SPREAD_TRIANGULAR 1u
#define MAX_QUAD_CHECK_POINTS 128u

typedef struct __attribute__((packed)) {
    int   base_block_x;
    int   base_block_z;
    ulong add_term_mod48;
    uint  bound;
    uint  constraint_index;
    uint  spread_type;
    uint  use_fast_next_int;
} RegionTerm;

typedef struct __attribute__((packed)) {
    uint  region_start;
    uint  region_count;
    ulong radius_sq;
    int   anchor_x;
    int   anchor_z;
    uint  gate_div;
    uint  gate_salt;
    uint  is_gate_only;
    uint  min_required;
    uint  quad_max_span;
} ConstraintDesc;

inline bool is_pow2_u32(uint v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

inline ulong lcg_step(ulong state) {
    return (state * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
}

inline uint next_int_exact(ulong *state, uint bound) {
    *state = lcg_step(*state);
    if (is_pow2_u32(bound)) {
        return (uint)(((ulong)bound * (*state >> 17)) >> 31);
    }
    uint bits = (uint)(*state >> 17);
    uint val  = bits % bound;
    while ((bits - val + (bound - 1u)) >= INT32_SIGN) {
        *state = lcg_step(*state);
        bits = (uint)(*state >> 17);
        val  = bits % bound;
    }
    return val;
}

inline bool region_hit_exact(
    __global const RegionTerm *term,
    ulong seed48,
    int anchor_x,
    int anchor_z,
    ulong radius_sq,
    int *out_x,
    int *out_z)
{
    ulong state = ((seed48 + term->add_term_mod48) ^ JAVA_MULT) & JAVA_MASK;
    uint bound = term->bound;
    if (bound == 0u) return false;

    uint off_x;
    uint off_z;
    if (term->spread_type == SPREAD_TRIANGULAR) {
        uint x1 = next_int_exact(&state, bound);
        uint x2 = next_int_exact(&state, bound);
        uint z1 = next_int_exact(&state, bound);
        uint z2 = next_int_exact(&state, bound);
        off_x = (x1 + x2) >> 1;
        off_z = (z1 + z2) >> 1;
    } else {
        off_x = next_int_exact(&state, bound);
        off_z = next_int_exact(&state, bound);
    }

    long bx = (long)term->base_block_x + (long)off_x * CHUNK_SIZE;
    long bz = (long)term->base_block_z + (long)off_z * CHUNK_SIZE;
    long dx = bx - (long)anchor_x;
    long dz = bz - (long)anchor_z;
    ulong d2 = (ulong)(dx * dx + dz * dz);
    if (d2 > radius_sq) return false;
    if (out_x != 0) *out_x = (int)bx;
    if (out_z != 0) *out_z = (int)bz;
    return true;
}

inline bool has_compact_group(
    const int *xs,
    const int *zs,
    uint count,
    uint min_required,
    uint max_span)
{
    if (count < min_required) return false;
    if (min_required <= 1u || max_span == 0u) return true;
    long span = (long)max_span;
    for (uint xi = 0; xi < count; ++xi) {
        long min_x = (long)xs[xi];
        long max_x = min_x + span;
        for (uint zi = 0; zi < count; ++zi) {
            long min_z = (long)zs[zi];
            long max_z = min_z + span;
            uint inside = 0u;
            for (uint k = 0; k < count; ++k) {
                long x = (long)xs[k];
                long z = (long)zs[k];
                if (x < min_x || x > max_x || z < min_z || z > max_z) continue;
                if (++inside >= min_required) return true;
            }
        }
    }
    return false;
}

__kernel void filter_one_min1(
    ulong start_seed,
    ulong count,
    ulong radius_sq,
    int   anchor_x,
    int   anchor_z,
    uint  region_start,
    uint  region_count,
    __global const RegionTerm *regions,
    __global ulong *out,
    __global uint *hit_count)
{
    ulong idx = get_global_id(0);
    if (idx >= count) return;
    ulong seed48 = (start_seed + idx) & JAVA_MASK;
    uint end = region_start + region_count;
    for (uint ri = region_start; ri < end; ++ri) {
        if (region_hit_exact(&regions[ri], seed48, anchor_x, anchor_z, radius_sq, 0, 0)) {
            uint pos = atomic_inc(hit_count);
            if ((ulong)pos < count) {
                out[pos] = seed48;
            }
            return;
        }
    }
}

__kernel void filter_multi_exact(
    ulong start_seed,
    ulong count,
    uint  constraint_count,
    __global const RegionTerm *regions,
    __global const ConstraintDesc *constraints,
    __global ulong *out,
    __global uint *hit_count)
{
    ulong idx = get_global_id(0);
    if (idx >= count) return;
    ulong seed48 = (start_seed + idx) & JAVA_MASK;

    for (uint ci = 0; ci < constraint_count; ++ci) {
        ConstraintDesc c = constraints[ci];
        if (c.is_gate_only != 0u || c.region_count == 0u) continue;

        uint min_required = (c.min_required == 0u) ? 1u : c.min_required;
        uint end = c.region_start + c.region_count;

        if (min_required == 1u && c.quad_max_span == 0u) {
            bool matched = false;
            for (uint ri = c.region_start; ri < end; ++ri) {
                if (region_hit_exact(&regions[ri], seed48, c.anchor_x, c.anchor_z, c.radius_sq, 0, 0)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) return;
            continue;
        }

        if (c.quad_max_span > 0u && min_required > 1u && c.region_count <= MAX_QUAD_CHECK_POINTS) {
            int mx[MAX_QUAD_CHECK_POINTS];
            int mz[MAX_QUAD_CHECK_POINTS];
            uint matched = 0u;
            for (uint ri = c.region_start; ri < end; ++ri) {
                int bx = 0;
                int bz = 0;
                bool hit = region_hit_exact(&regions[ri], seed48, c.anchor_x, c.anchor_z, c.radius_sq, &bx, &bz);
                if (hit) {
                    if (matched < MAX_QUAD_CHECK_POINTS) {
                        mx[matched] = bx;
                        mz[matched] = bz;
                    }
                    matched += 1u;
                }
                uint remaining = end - (ri + 1u);
                if (matched + remaining < min_required) return;
            }
            if (matched < min_required) return;
            if (!has_compact_group(mx, mz, matched, min_required, c.quad_max_span)) return;
            continue;
        }

        uint matched = 0u;
        for (uint ri = c.region_start; ri < end; ++ri) {
            bool hit = region_hit_exact(&regions[ri], seed48, c.anchor_x, c.anchor_z, c.radius_sq, 0, 0);
            if (hit) matched += 1u;
            if (matched >= min_required) break;
            uint remaining = end - (ri + 1u);
            if (matched + remaining < min_required) return;
        }
        if (matched < min_required) return;
    }

    uint pos = atomic_inc(hit_count);
    if ((ulong)pos < count) {
        out[pos] = seed48;
    }
}
)OCL";

// ============================================================================
// Device context. One persistent context, command queue, program and
// kernels for the first GPU. Buffers grow on demand.
// ============================================================================

struct DeviceContext {
    cl_platform_id   platform = nullptr;
    cl_device_id     device   = nullptr;
    cl_context       context  = nullptr;
    cl_command_queue queue    = nullptr;
    cl_program       program  = nullptr;
    cl_kernel        kernel_one_min1 = nullptr;
    cl_kernel        kernel_multi    = nullptr;

    cl_mem regions_buf      = nullptr;
    size_t regions_capacity = 0;
    cl_mem constraints_buf      = nullptr;
    size_t constraints_capacity = 0;
    cl_mem out_buf      = nullptr;
    size_t out_capacity = 0;
    cl_mem hit_buf      = nullptr;

    size_t preferred_work_group_size = 64;
    cl_ulong device_global_mem = 0;

    bool initialized = false;
    bool init_failed = false;
};

std::mutex g_ctx_mu;
DeviceContext g_ctx;

void log_build_error(cl_program program, cl_device_id device) {
    OpenCLApi &api = cl_api();
    size_t log_size = 0;
    api.GetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
    if (log_size == 0) return;
    std::string log(log_size, '\0');
    api.GetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, log.data(), nullptr);
    std::fprintf(stderr, "[gpu_filter_opencl] Kernel build log:\n%s\n", log.c_str());
}

bool pick_device(cl_platform_id *out_platform, cl_device_id *out_device) {
    OpenCLApi &api = cl_api();
    cl_uint platform_count = 0;
    if (api.GetPlatformIDs(0, nullptr, &platform_count) != CL_SUCCESS || platform_count == 0) {
        return false;
    }
    std::vector<cl_platform_id> platforms(platform_count);
    if (api.GetPlatformIDs(platform_count, platforms.data(), nullptr) != CL_SUCCESS) {
        return false;
    }
    // Prefer the first platform that has a GPU. Fall back to first with any
    // device. Lets users with both an iGPU and a discrete card auto-select.
    cl_platform_id best_platform = nullptr;
    cl_device_id best_device = nullptr;
    for (cl_platform_id p : platforms) {
        cl_uint device_count = 0;
        if (api.GetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 0, nullptr, &device_count) == CL_SUCCESS && device_count > 0) {
            std::vector<cl_device_id> devices(device_count);
            if (api.GetDeviceIDs(p, CL_DEVICE_TYPE_GPU, device_count, devices.data(), nullptr) == CL_SUCCESS) {
                best_platform = p;
                best_device = devices[0];
                break;
            }
        }
    }
    if (best_device == nullptr) {
        for (cl_platform_id p : platforms) {
            cl_uint device_count = 0;
            if (api.GetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count) == CL_SUCCESS && device_count > 0) {
                std::vector<cl_device_id> devices(device_count);
                if (api.GetDeviceIDs(p, CL_DEVICE_TYPE_ALL, device_count, devices.data(), nullptr) == CL_SUCCESS) {
                    best_platform = p;
                    best_device = devices[0];
                    break;
                }
            }
        }
    }
    if (best_device == nullptr) return false;
    *out_platform = best_platform;
    *out_device = best_device;
    return true;
}

bool ensure_initialized() {
    std::lock_guard<std::mutex> lock(g_ctx_mu);
    if (g_ctx.initialized) return true;
    if (g_ctx.init_failed) return false;
    OpenCLApi &api = cl_api();
    if (!api.ready) {
        g_ctx.init_failed = true;
        return false;
    }
    if (!pick_device(&g_ctx.platform, &g_ctx.device)) {
        g_ctx.init_failed = true;
        return false;
    }

    cl_int err = CL_SUCCESS;
    g_ctx.context = api.CreateContext(nullptr, 1, &g_ctx.device, nullptr, nullptr, &err);
    if (g_ctx.context == nullptr || err != CL_SUCCESS) {
        g_ctx.init_failed = true;
        return false;
    }

    if (api.CreateCommandQueueWithProperties != nullptr) {
        cl_queue_properties props[] = {0};
        g_ctx.queue = api.CreateCommandQueueWithProperties(g_ctx.context, g_ctx.device, props, &err);
    } else if (api.CreateCommandQueue != nullptr) {
        g_ctx.queue = api.CreateCommandQueue(g_ctx.context, g_ctx.device, 0, &err);
    }
    if (g_ctx.queue == nullptr || err != CL_SUCCESS) {
        api.ReleaseContext(g_ctx.context);
        g_ctx.context = nullptr;
        g_ctx.init_failed = true;
        return false;
    }

    const char *src_ptr = kKernelSource;
    size_t src_len = std::strlen(kKernelSource);
    g_ctx.program = api.CreateProgramWithSource(g_ctx.context, 1, &src_ptr, &src_len, &err);
    if (g_ctx.program == nullptr || err != CL_SUCCESS) {
        api.ReleaseCommandQueue(g_ctx.queue);
        api.ReleaseContext(g_ctx.context);
        g_ctx.queue = nullptr;
        g_ctx.context = nullptr;
        g_ctx.init_failed = true;
        return false;
    }
    const char *build_opts = "-cl-std=CL1.2 -cl-fast-relaxed-math";
    err = api.BuildProgram(g_ctx.program, 1, &g_ctx.device, build_opts, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        log_build_error(g_ctx.program, g_ctx.device);
        api.ReleaseProgram(g_ctx.program);
        api.ReleaseCommandQueue(g_ctx.queue);
        api.ReleaseContext(g_ctx.context);
        g_ctx.program = nullptr;
        g_ctx.queue = nullptr;
        g_ctx.context = nullptr;
        g_ctx.init_failed = true;
        return false;
    }

    g_ctx.kernel_one_min1 = api.CreateKernel(g_ctx.program, "filter_one_min1", &err);
    if (g_ctx.kernel_one_min1 == nullptr || err != CL_SUCCESS) {
        g_ctx.init_failed = true;
        return false;
    }
    g_ctx.kernel_multi = api.CreateKernel(g_ctx.program, "filter_multi_exact", &err);
    if (g_ctx.kernel_multi == nullptr || err != CL_SUCCESS) {
        g_ctx.init_failed = true;
        return false;
    }

    size_t info_size = 0;
    api.GetDeviceInfo(g_ctx.device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(info_size), &info_size, nullptr);
    if (info_size > 0) {
        g_ctx.preferred_work_group_size = std::min<size_t>(info_size, 256);
    }
    api.GetDeviceInfo(g_ctx.device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(g_ctx.device_global_mem),
                      &g_ctx.device_global_mem, nullptr);
    g_ctx.hit_buf = api.CreateBuffer(g_ctx.context, CL_MEM_READ_WRITE, sizeof(uint32_t), nullptr, &err);
    if (g_ctx.hit_buf == nullptr || err != CL_SUCCESS) {
        g_ctx.init_failed = true;
        return false;
    }

    g_ctx.initialized = true;
    return true;
}

bool ensure_buffer(cl_mem *buf, size_t *capacity, size_t needed, cl_mem_flags flags) {
    if (*buf != nullptr && *capacity >= needed) return true;
    OpenCLApi &api = cl_api();
    if (*buf != nullptr) {
        api.ReleaseMemObject(*buf);
        *buf = nullptr;
        *capacity = 0;
    }
    size_t new_cap = std::max<size_t>(needed, *capacity + (*capacity >> 1));
    new_cap = std::max<size_t>(new_cap, 1);
    cl_int err = CL_SUCCESS;
    *buf = api.CreateBuffer(g_ctx.context, flags, new_cap, nullptr, &err);
    if (*buf == nullptr || err != CL_SUCCESS) {
        return false;
    }
    *capacity = new_cap;
    return true;
}

}  // namespace

// ============================================================================
// Public ABI — mirrors cuda/gpu_filter_wrapper.cpp.
// ============================================================================

extern "C" GPU_FILTER_API int gpu_is_available(void) {
    OpenCLApi &api = cl_api();
    if (!api.ready) return 0;
    cl_uint platform_count = 0;
    if (api.GetPlatformIDs(0, nullptr, &platform_count) != CL_SUCCESS) return 0;
    return platform_count > 0 ? 1 : 0;
}

extern "C" GPU_FILTER_API uint64_t gpu_total_mem(void) {
    if (!ensure_initialized()) return 0;
    return static_cast<uint64_t>(g_ctx.device_global_mem);
}

extern "C" GPU_FILTER_API int gpu_filter_multi_checked(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count)
{
    if (hit_count == nullptr) return -1;
    *hit_count = 0;
    if (count == 0 || output_buffer == nullptr || constraints == nullptr || constraint_count == 0) {
        return -1;
    }
    if (!ensure_initialized()) return -2;

    // Drop gate-only entries; this backend only handles structure constraints
    // (matching the CUDA backend's contract).
    std::vector<ConstraintDesc> exact_constraints;
    exact_constraints.reserve(constraint_count);
    for (uint32_t i = 0; i < constraint_count; ++i) {
        const ConstraintDesc c = constraints[i];
        if (c.is_gate_only != 0u || c.region_count == 0u) continue;
        exact_constraints.push_back(c);
    }
    if (exact_constraints.empty()) {
        // No structure work — emit the entire input range as low48 seeds, the
        // same passthrough behavior the CUDA backend uses.
        for (uint64_t i = 0; i < count; ++i) {
            output_buffer[i] = (start_seed + i) & ((1ULL << 48) - 1ULL);
        }
        *hit_count = static_cast<uint32_t>(std::min<uint64_t>(count, UINT32_MAX));
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_ctx_mu);
    OpenCLApi &api = cl_api();

    const size_t regions_bytes = static_cast<size_t>(region_count) * sizeof(RegionTerm);
    const size_t constraints_bytes = exact_constraints.size() * sizeof(ConstraintDesc);
    const size_t out_bytes = static_cast<size_t>(count) * sizeof(uint64_t);

    if (region_count > 0 && !ensure_buffer(&g_ctx.regions_buf, &g_ctx.regions_capacity, regions_bytes, CL_MEM_READ_ONLY)) {
        return -5;
    }
    if (!ensure_buffer(&g_ctx.constraints_buf, &g_ctx.constraints_capacity, constraints_bytes, CL_MEM_READ_ONLY)) {
        return -5;
    }
    if (!ensure_buffer(&g_ctx.out_buf, &g_ctx.out_capacity, out_bytes, CL_MEM_WRITE_ONLY)) {
        return -5;
    }

    cl_int err = CL_SUCCESS;
    if (region_count > 0) {
        err = api.EnqueueWriteBuffer(g_ctx.queue, g_ctx.regions_buf, CL_FALSE, 0, regions_bytes,
                                     regions, 0, nullptr, nullptr);
        if (err != CL_SUCCESS) return -6;
    }
    err = api.EnqueueWriteBuffer(g_ctx.queue, g_ctx.constraints_buf, CL_FALSE, 0, constraints_bytes,
                                 exact_constraints.data(), 0, nullptr, nullptr);
    if (err != CL_SUCCESS) return -6;

    uint32_t zero = 0;
    err = api.EnqueueWriteBuffer(g_ctx.queue, g_ctx.hit_buf, CL_FALSE, 0, sizeof(uint32_t),
                                 &zero, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) return -6;

    const bool use_single =
        exact_constraints.size() == 1 &&
        (exact_constraints[0].min_required == 0u || exact_constraints[0].min_required == 1u) &&
        exact_constraints[0].quad_max_span == 0u;

    cl_kernel kernel = use_single ? g_ctx.kernel_one_min1 : g_ctx.kernel_multi;
    cl_uint arg = 0;
    err |= api.SetKernelArg(kernel, arg++, sizeof(uint64_t), &start_seed);
    err |= api.SetKernelArg(kernel, arg++, sizeof(uint64_t), &count);
    if (use_single) {
        const ConstraintDesc &c = exact_constraints[0];
        err |= api.SetKernelArg(kernel, arg++, sizeof(uint64_t), &c.radius_sq);
        err |= api.SetKernelArg(kernel, arg++, sizeof(int32_t),  &c.anchor_x);
        err |= api.SetKernelArg(kernel, arg++, sizeof(int32_t),  &c.anchor_z);
        err |= api.SetKernelArg(kernel, arg++, sizeof(uint32_t), &c.region_start);
        err |= api.SetKernelArg(kernel, arg++, sizeof(uint32_t), &c.region_count);
        err |= api.SetKernelArg(kernel, arg++, sizeof(cl_mem),   &g_ctx.regions_buf);
        err |= api.SetKernelArg(kernel, arg++, sizeof(cl_mem),   &g_ctx.out_buf);
        err |= api.SetKernelArg(kernel, arg++, sizeof(cl_mem),   &g_ctx.hit_buf);
    } else {
        const uint32_t cc = static_cast<uint32_t>(exact_constraints.size());
        err |= api.SetKernelArg(kernel, arg++, sizeof(uint32_t), &cc);
        err |= api.SetKernelArg(kernel, arg++, sizeof(cl_mem),   &g_ctx.regions_buf);
        err |= api.SetKernelArg(kernel, arg++, sizeof(cl_mem),   &g_ctx.constraints_buf);
        err |= api.SetKernelArg(kernel, arg++, sizeof(cl_mem),   &g_ctx.out_buf);
        err |= api.SetKernelArg(kernel, arg++, sizeof(cl_mem),   &g_ctx.hit_buf);
    }
    if (err != CL_SUCCESS) return -7;

    size_t local = g_ctx.preferred_work_group_size > 0 ? g_ctx.preferred_work_group_size : 64;
    if (local > 256) local = 256;
    // Round global up to a multiple of local so OpenCL 1.2 is happy. The
    // kernel guards on idx>=count.
    size_t global = (static_cast<size_t>(count) + local - 1) / local * local;
    err = api.EnqueueNDRangeKernel(g_ctx.queue, kernel, 1, nullptr, &global, &local,
                                   0, nullptr, nullptr);
    if (err != CL_SUCCESS) return -7;

    uint32_t produced = 0;
    err = api.EnqueueReadBuffer(g_ctx.queue, g_ctx.hit_buf, CL_TRUE, 0, sizeof(uint32_t),
                                &produced, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) return -6;
    if (produced > count) produced = static_cast<uint32_t>(count);
    if (produced > 0) {
        err = api.EnqueueReadBuffer(g_ctx.queue, g_ctx.out_buf, CL_TRUE, 0,
                                    static_cast<size_t>(produced) * sizeof(uint64_t),
                                    output_buffer, 0, nullptr, nullptr);
        if (err != CL_SUCCESS) return -6;
    }
    *hit_count = produced;
    return 0;
}

extern "C" GPU_FILTER_API void gpu_filter_multi(
    uint64_t start_seed,
    uint64_t count,
    RegionTerm *regions,
    uint32_t region_count,
    ConstraintDesc *constraints,
    uint32_t constraint_count,
    uint64_t *output_buffer,
    uint32_t *hit_count)
{
    (void)gpu_filter_multi_checked(start_seed, count, regions, region_count,
                                   constraints, constraint_count,
                                   output_buffer, hit_count);
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
    uint32_t *hit_count)
{
    // Translate the legacy single-constraint entry point into a one-shot
    // multi-constraint call so the OpenCL kernel handles both paths.
    if (hit_count == nullptr) return;
    *hit_count = 0;
    if (count == 0 || output_buffer == nullptr || regions == nullptr || region_count <= 0) {
        return;
    }
    ConstraintDesc c{};
    c.region_start = 0;
    c.region_count = static_cast<uint32_t>(region_count);
    c.radius_sq = radius_sq;
    c.anchor_x = 0;
    c.anchor_z = 0;
    c.gate_div = gate_div;
    c.gate_salt = gate_salt;
    c.is_gate_only = 0;
    c.min_required = 1;
    c.quad_max_span = 0;
    (void)gpu_filter_multi_checked(start_seed, count, regions,
                                   static_cast<uint32_t>(region_count),
                                   &c, 1u, output_buffer, hit_count);
}

// The OpenCL backend ships without the double-buffer async API. The scanner
// gracefully falls back to the sync path when this returns 0 (or when the
// submit/collect symbols are absent — we expose neither to make that
// definitive).
extern "C" GPU_FILTER_API int gpu_filter_double_buffer_available(void) {
    return 0;
}
