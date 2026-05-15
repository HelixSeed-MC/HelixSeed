import ctypes
import sys
from pathlib import Path


class RegionTerm(ctypes.Structure):
    _fields_ = [
        ("base_block_x", ctypes.c_int32),
        ("base_block_z", ctypes.c_int32),
        ("add_term_mod48", ctypes.c_uint64),
        ("bound", ctypes.c_uint32),
        ("constraint_index", ctypes.c_uint32),
        ("spread_type", ctypes.c_uint32),
        ("use_fast_next_int", ctypes.c_uint32),
    ]


class ConstraintDesc(ctypes.Structure):
    _fields_ = [
        ("region_start", ctypes.c_uint32),
        ("region_count", ctypes.c_uint32),
        ("radius_sq", ctypes.c_uint64),
        ("anchor_x", ctypes.c_int32),
        ("anchor_z", ctypes.c_int32),
        ("gate_div", ctypes.c_uint32),
        ("gate_salt", ctypes.c_uint32),
        ("is_gate_only", ctypes.c_uint32),
        ("min_required", ctypes.c_uint32),
        ("quad_max_span", ctypes.c_uint32),
    ]


def main() -> None:
    lib_name = "gpu_filter.dll" if sys.platform.startswith("win") else "libgpu_filter.so"
    dll = Path(__file__).resolve().with_name(lib_name)
    if not dll.exists():
        dll = Path(__file__).resolve().parents[1] / lib_name
    lib = ctypes.CDLL(str(dll))

    lib.gpu_filter_multi.restype = None
    lib.gpu_filter_multi.argtypes = [
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.POINTER(RegionTerm),
        ctypes.c_uint32,
        ctypes.POINTER(ConstraintDesc),
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint64),
        ctypes.POINTER(ctypes.c_uint32),
    ]

    # Example: one linear village-like region term.
    regions = (RegionTerm * 1)(
        RegionTerm(
            base_block_x=0,
            base_block_z=0,
            add_term_mod48=10387312,
            bound=26,  # spacing-separation
            constraint_index=0,
            spread_type=0,  # linear
            use_fast_next_int=0,  # exact rejection logic
        )
    )
    constraints = (ConstraintDesc * 1)(
        ConstraintDesc(
            region_start=0,
            region_count=1,
            radius_sq=200 * 200,
            anchor_x=0,
            anchor_z=0,
            gate_div=1,
            gate_salt=0,
            is_gate_only=0,
            min_required=1,
        )
    )

    count = 100000
    out = (ctypes.c_uint64 * count)()
    hit = ctypes.c_uint32(0)

    lib.gpu_filter_multi(
        ctypes.c_uint64(0),
        ctypes.c_uint64(count),
        regions,
        ctypes.c_uint32(len(regions)),
        constraints,
        ctypes.c_uint32(len(constraints)),
        out,
        ctypes.byref(hit),
    )

    print(f"hits={hit.value}")
    for i in range(min(10, hit.value)):
        print(int(out[i]))


if __name__ == "__main__":
    main()
