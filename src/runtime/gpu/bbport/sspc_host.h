/* sspc-integration: the GPU library (bbgpu-sspc.dll) hosted by SecondSonPC instead of bbport's loader.
 * The loader functions the library takes (runtime_* in bbport) come from the host through this table;
 * everything crossing it is plain C (the DLL is built by MinGW clang/libc++, the host by clang-cl/MSVC STL;
 * both use the UCRT). Shared verbatim with SecondSonPC (src/runtime/gpu/bbport/sspc_host.h). */
#ifndef BBGPU_SSPC_HOST_H
#define BBGPU_SSPC_HOST_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define BBGPU_SSPC_HOST_VERSION 1

typedef void (*BbSspcRange)(uintptr_t address, uint64_t size);

typedef struct BbSspcHost {
    uint32_t version;   /* BBGPU_SSPC_HOST_VERSION */
    uint32_t reserved;
    /* guest memory (bbport runtime_memory.c semantics) */
    int (*write_backing)(uintptr_t address, const void* data, uint64_t size);
    uint64_t (*clamp)(uintptr_t address, uint64_t size);
    int (*region)(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
    const uint64_t* (*generation)(void);
    void (*gpu_protect)(uintptr_t address, uint64_t size, int read, int write);
    int (*vma_info)(uintptr_t address, int* prot, int* type, uintptr_t* end);
    int (*direct_phys)(uintptr_t address, uint64_t* phys, uintptr_t* end);
    int (*is_mapped)(uintptr_t address, uint64_t size);
    /* map / unmap / invalidate: the host calls them for every change of guest memory from now on, after
     * calling map for every existing mapping */
    void (*set_gpu_hooks)(BbSspcRange map, BbSspcRange unmap, BbSspcRange invalidate);
    /* time: one clock for guest and GPU timestamps */
    uint64_t (*process_time_us)(void);
    uint64_t (*process_time_counter)(void);
    uint64_t (*tsc_frequency)(void);
    int32_t* (*guest_errno)(void);
    void (*restart)(void);
    void (*log)(const char* line); /* the library's log lines (may be null) */
} BbSspcHost;

/* Exported by bbgpu-sspc.dll. */
typedef int (*BbSspcSetHostFn)(const BbSspcHost* host);
/* An access violation the host's exception dispatch received (EXCEPTION_POINTERS*): 1 when the library's GPU page
 * tracking handled it (the faulting access retries; the context may have been changed). */
typedef int (*BbSspcExceptionFn)(void* exception_pointers);

#ifdef __cplusplus
}
#endif
#endif
