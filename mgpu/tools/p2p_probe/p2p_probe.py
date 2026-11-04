"""HIP peer-to-peer probe for two GPUs, using only the driver's amdhip64_7.dll.

Reports hipDeviceCanAccessPeer, hipDeviceGetP2PAttribute, the large-BAR
attribute, tries hipDeviceEnablePeerAccess, and times hipMemcpyPeer against
plain host-link copies (HtoD/DtoH) on each device.
"""
import ctypes as C
import sys
import time

hip = C.WinDLL("amdhip64_7.dll")
i32, sz, vp = C.c_int, C.c_size_t, C.c_void_p

ATTR_IS_LARGE_BAR = 10011  # hipDeviceAttributeIsLargeBar; 10020 = PciChipId anchors the enum
P2P_ATTRS = {"PerformanceRank": 0, "AccessSupported": 1, "NativeAtomicSupported": 2, "HipArrayAccessSupported": 3}
MB = 1 << 20
SIZE = 256 * MB
ITERS = 5


def err(e):
    hip.hipGetErrorString.restype = C.c_char_p
    return f"{e} ({hip.hipGetErrorString(e).decode()})"


def ok(e, what):
    if e != 0:
        raise RuntimeError(f"{what}: {err(e)}")


def bus_id(d):
    buf = C.create_string_buffer(64)
    hip.hipDeviceGetPCIBusId(buf, 64, d)
    return buf.value.decode()


def timed(fn):
    best = 1e9
    for _ in range(ITERS):
        t = time.perf_counter()
        fn()
        best = min(best, time.perf_counter() - t)
    return SIZE / best / 1e9, best * 1e3


def main():
    n = i32()
    ok(hip.hipGetDeviceCount(C.byref(n)), "hipGetDeviceCount")
    print(f"devices: {n.value}")
    if n.value < 2:
        return 1
    for d in range(2):
        v = i32(-1)
        e = hip.hipDeviceGetAttribute(C.byref(v), ATTR_IS_LARGE_BAR, d)
        c = i32(-1)
        hip.hipDeviceGetAttribute(C.byref(c), 10020, d)
        print(f"dev{d} bus {bus_id(d)} isLargeBar={v.value} (err {e}) pciChipId=0x{c.value:04x}")
    for a, b in ((0, 1), (1, 0)):
        can = i32(-1)
        e = hip.hipDeviceCanAccessPeer(C.byref(can), a, b)
        attrs = {}
        for k, code in P2P_ATTRS.items():
            v = i32(-1)
            r = hip.hipDeviceGetP2PAttribute(C.byref(v), code, a, b)
            attrs[k] = v.value if r == 0 else f"err {r}"
        print(f"canAccessPeer {a}->{b} = {can.value} (err {e}); P2P attrs {attrs}")
        ok(hip.hipSetDevice(a), "hipSetDevice")
        e = hip.hipDeviceEnablePeerAccess(b, 0)
        print(f"enablePeerAccess {a}->{b}: {err(e)}")

    host = vp()
    ok(hip.hipHostMalloc(C.byref(host), sz(SIZE), 0), "hipHostMalloc")
    bufs = []
    for d in range(2):
        ok(hip.hipSetDevice(d), "hipSetDevice")
        p = vp()
        ok(hip.hipMalloc(C.byref(p), sz(SIZE)), "hipMalloc")
        bufs.append(p)

    for d in range(2):
        ok(hip.hipSetDevice(d), "hipSetDevice")
        h2d = timed(lambda: ok(hip.hipMemcpy(bufs[d], host, sz(SIZE), 1), "HtoD"))
        d2h = timed(lambda: ok(hip.hipMemcpy(host, bufs[d], sz(SIZE), 2), "DtoH"))
        print(f"dev{d} pinned HtoD {h2d[0]:.1f} GB/s, DtoH {d2h[0]:.1f} GB/s")

    for a, b in ((0, 1), (1, 0)):
        ok(hip.hipSetDevice(a), "hipSetDevice")
        def cp():
            ok(hip.hipMemcpyPeer(bufs[b], b, bufs[a], a, sz(SIZE)), "hipMemcpyPeer")
            ok(hip.hipDeviceSynchronize(), "sync")
        gbs, ms = timed(cp)
        print(f"hipMemcpyPeer dev{a}->dev{b} 256 MiB: {gbs:.1f} GB/s ({ms:.1f} ms best of {ITERS})")

    for d in range(2):
        hip.hipSetDevice(d)
        hip.hipFree(bufs[d])
    hip.hipHostFree(host)
    return 0


if __name__ == "__main__":
    sys.exit(main())
