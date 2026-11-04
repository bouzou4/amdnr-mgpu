// xapi_probe: feasibility probes for multi-GPU NR on D3D11 and Vulkan games (render card without monitors,
// display card with them). Prints one RESULT line per probe.
//   xapi_probe.exe [func|bench] [d3d|vk|all]
//   func  (default) small allocations: what can be shared, opened, imported and signalled across cards
//   bench 4K R10G10B10A2 copies per send path (needs idle GPUs: take the lock queue first)
#include "common.h"

int main(int argc, char** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool bench = argc > 1 && strcmp(argv[1], "bench") == 0;
    const char* which = argc > 2 ? argv[2] : "all";
    Adapter r, d;
    if (!PickAdapters(r, d))
    {
        printf("need an AMD card without outputs and one with outputs\n");
        return 1;
    }
    printf("render card bus %d, display card bus %d, mode %s\n", r.bus, d.bus, bench ? "bench" : "func");
    if (strcmp(which, "vk") != 0)
        ProbesD3D(r, d, bench);
    if (strcmp(which, "d3d") != 0)
        ProbesVk(r, d, bench);
    return 0;
}
