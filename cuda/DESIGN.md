# CUDA Kernel Design Notes

## Threading model
- One thread per seed.
- 256 threads per block (`THREADS_PER_BLOCK=256`) for Ampere-friendly occupancy.
- Host launches in chunks (`MAX_SEEDS_PER_LAUNCH=16M`) for large `count` values.

## Determinism and correctness
- Java 48-bit LCG implemented with exact mask after each step.
- Exact `nextInt(bound)` rejection logic matches signed-32 overflow behavior.
- Fast prefilter `nextInt` variant preserves old behavior (single draw modulo for non-power-of-two bounds).
- Triangular spread does four draws and averages pairs exactly as prior Python path.
- Distance test uses integer math only (`int64` -> `uint64` compare with `radius_sq`).

## Constraint pipeline
- `gpu_filter` = single-constraint OR across regions + optional gate.
- `gpu_filter_multi` = AND across constraints:
  - gate test per constraint first
  - optional gate-only constraints
  - per-constraint region OR test
  - early thread exit on first failing constraint

## Memory strategy
- Region and constraint descriptors are copied into constant memory:
  - `k_regions[MAX_CONST_REGIONS]`
  - `k_constraints[MAX_CONST_CONSTRAINTS]`
- Passing seeds are compacted via `atomicAdd(hit_count, 1)` into output buffer.
- No dynamic allocation occurs inside kernels.

## Branch and math optimizations
- Power-of-two checks switch `%` to bitmask where possible (gate and `nextInt`).
- Fast/Exact `nextInt` selected by region flag (`use_fast_next_int`) to preserve surrogate behavior.
- Kernel exits immediately on gate/constraint failure to reduce wasted work.

## Limits
- Current constant-memory capacities:
  - max regions: 2048
  - max constraints: 256
- Exceeding limits currently returns zero hits (host-side guard path).
