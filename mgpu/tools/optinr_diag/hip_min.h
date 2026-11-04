// Minimal HIP runtime declarations for dynamic loading of amdhip64_7.dll.
// Layouts follow AMD's MIT-licensed HIP headers (ROCm/hip, rocm-7.0.0); see THIRD_PARTY_NOTICES.md.
#pragma once
#include <cstddef>
#include <cstdint>

using hipError_t = int;
using hipStream_t = struct ihipStream_t*;
using hipEvent_t = struct ihipEvent_t*;
using hipExternalMemory_t = void*;
using hipExternalSemaphore_t = void*;
using hipDeviceptr_t = void*;

constexpr hipError_t hipSuccess = 0;
constexpr unsigned hipExternalMemoryDedicated = 0x1;
enum hipMemcpyKind { hipMemcpyHostToHost = 0, hipMemcpyHostToDevice = 1, hipMemcpyDeviceToHost = 2, hipMemcpyDeviceToDevice = 3 };

enum hipExternalMemoryHandleType {
  hipExternalMemoryHandleTypeOpaqueWin32 = 2,
  hipExternalMemoryHandleTypeD3D12Heap = 4,
  hipExternalMemoryHandleTypeD3D12Resource = 5,
};
struct hipExternalMemoryHandleDesc {
  hipExternalMemoryHandleType type;
  union {
    int fd;
    struct { void* handle; const void* name; } win32;
    const void* nvSciBufObject;
  } handle;
  unsigned long long size;
  unsigned int flags;
  unsigned int reserved[16];
};
struct hipExternalMemoryBufferDesc {
  unsigned long long offset;
  unsigned long long size;
  unsigned int flags;
  unsigned int reserved[16];
};

enum hipExternalSemaphoreHandleType { hipExternalSemaphoreHandleTypeD3D12Fence = 4 };
struct hipExternalSemaphoreHandleDesc {
  hipExternalSemaphoreHandleType type;
  union {
    int fd;
    struct { void* handle; const void* name; } win32;
    const void* NvSciSyncObj;
  } handle;
  unsigned int flags;
  unsigned int reserved[16];
};
struct hipExternalSemaphoreSignalParams {
  struct {
    struct { unsigned long long value; } fence;
    union { void* fence; unsigned long long reserved; } nvSciSync;
    struct { unsigned long long key; } keyedMutex;
    unsigned int reserved[12];
  } params;
  unsigned int flags;
  unsigned int reserved[16];
};
struct hipExternalSemaphoreWaitParams {
  struct {
    struct { unsigned long long value; } fence;
    union { void* fence; unsigned long long reserved; } nvSciSync;
    struct { unsigned long long key; unsigned int timeoutMs; } keyedMutex;
    unsigned int reserved[10];
  } params;
  unsigned int flags;
  unsigned int reserved[16];
};

// hipDeviceProp_t as exported by hipGetDevicePropertiesR0600 (HIP 6.0+).
struct HipDevicePropR0600 {
    char name[256];
    char uuid[16];
    char luid[8];
    unsigned int luidDeviceNodeMask;
    size_t totalGlobalMem;
    size_t sharedMemPerBlock;
    int regsPerBlock;
    int warpSize;
    size_t memPitch;
    int maxThreadsPerBlock;
    int maxThreadsDim[3];
    int maxGridSize[3];
    int clockRate;
    size_t totalConstMem;
    int major;
    int minor;
    size_t textureAlignment;
    size_t texturePitchAlignment;
    int deviceOverlap;
    int multiProcessorCount;
    int kernelExecTimeoutEnabled;
    int integrated;
    int canMapHostMemory;
    int computeMode;
    int maxTexture1D;
    int maxTexture1DMipmap;
    int maxTexture1DLinear;
    int maxTexture2D[2];
    int maxTexture2DMipmap[2];
    int maxTexture2DLinear[3];
    int maxTexture2DGather[2];
    int maxTexture3D[3];
    int maxTexture3DAlt[3];
    int maxTextureCubemap;
    int maxTexture1DLayered[2];
    int maxTexture2DLayered[3];
    int maxTextureCubemapLayered[2];
    int maxSurface1D;
    int maxSurface2D[2];
    int maxSurface3D[3];
    int maxSurface1DLayered[2];
    int maxSurface2DLayered[3];
    int maxSurfaceCubemap;
    int maxSurfaceCubemapLayered[2];
    size_t surfaceAlignment;
    int concurrentKernels;
    int ECCEnabled;
    int pciBusID;
    int pciDeviceID;
    int pciDomainID;
    int tccDriver;
    int asyncEngineCount;
    int unifiedAddressing;
    int memoryClockRate;
    int memoryBusWidth;
    int l2CacheSize;
    int persistingL2CacheMaxSize;
    int maxThreadsPerMultiProcessor;
    int streamPrioritiesSupported;
    int globalL1CacheSupported;
    int localL1CacheSupported;
    size_t sharedMemPerMultiprocessor;
    int regsPerMultiprocessor;
    int managedMemory;
    int isMultiGpuBoard;
    int multiGpuBoardGroupID;
    int hostNativeAtomicSupported;
    int singleToDoublePrecisionPerfRatio;
    int pageableMemoryAccess;
    int concurrentManagedAccess;
    int computePreemptionSupported;
    int canUseHostPointerForRegisteredMem;
    int cooperativeLaunch;
    int cooperativeMultiDeviceLaunch;
    size_t sharedMemPerBlockOptin;
    int pageableMemoryAccessUsesHostPageTables;
    int directManagedMemAccessFromHost;
    int maxBlocksPerMultiProcessor;
    int accessPolicyMaxWindowSize;
    size_t reservedSharedMemPerBlock;
    int hostRegisterSupported;
    int sparseHipArraySupported;
    int hostRegisterReadOnlySupported;
    int timelineSemaphoreInteropSupported;
    int memoryPoolsSupported;
    int gpuDirectRDMASupported;
    unsigned int gpuDirectRDMAFlushWritesOptions;
    int gpuDirectRDMAWritesOrdering;
    unsigned int memoryPoolSupportedHandleTypes;
    int deferredMappingHipArraySupported;
    int ipcEventSupported;
    int clusterLaunch;
    int unifiedFunctionPointers;
    int reserved[63];
    int hipReserved[32];
    char gcnArchName[256];
    size_t maxSharedMemoryPerMultiProcessor;
    int clockInstructionRate;
    unsigned arch;
    unsigned int* hdpMemFlushCntl;
    unsigned int* hdpRegFlushCntl;
    int cooperativeMultiDeviceUnmatchedFunc;
    int cooperativeMultiDeviceUnmatchedGridDim;
    int cooperativeMultiDeviceUnmatchedBlockDim;
    int cooperativeMultiDeviceUnmatchedSharedMem;
    int isLargeBar;
    int asicRevision;
};
    static_assert(offsetof(HipDevicePropR0600, luid) == 272);
static_assert(offsetof(HipDevicePropR0600, totalGlobalMem) == 288);

#define OPTINR_HIP_FUNCS(X) \
  X(hipInit, hipError_t, (unsigned)) \
  X(hipDriverGetVersion, hipError_t, (int*)) \
  X(hipRuntimeGetVersion, hipError_t, (int*)) \
  X(hipGetErrorString, const char*, (hipError_t)) \
  X(hipGetDeviceCount, hipError_t, (int*)) \
  X(hipGetDevicePropertiesR0600, hipError_t, (HipDevicePropR0600*, int)) \
  X(hipDeviceGetPCIBusId, hipError_t, (char*, int, int)) \
  X(hipSetDevice, hipError_t, (int)) \
  X(hipDeviceSynchronize, hipError_t, ()) \
  X(hipStreamCreate, hipError_t, (hipStream_t*)) \
  X(hipStreamDestroy, hipError_t, (hipStream_t)) \
  X(hipStreamSynchronize, hipError_t, (hipStream_t)) \
  X(hipEventCreate, hipError_t, (hipEvent_t*)) \
  X(hipEventDestroy, hipError_t, (hipEvent_t)) \
  X(hipEventRecord, hipError_t, (hipEvent_t, hipStream_t)) \
  X(hipEventSynchronize, hipError_t, (hipEvent_t)) \
  X(hipEventElapsedTime, hipError_t, (float*, hipEvent_t, hipEvent_t)) \
  X(hipMalloc, hipError_t, (void**, size_t)) \
  X(hipFree, hipError_t, (void*)) \
  X(hipMemcpy, hipError_t, (void*, const void*, size_t, hipMemcpyKind)) \
  X(hipMemcpyAsync, hipError_t, (void*, const void*, size_t, hipMemcpyKind, hipStream_t)) \
  X(hipMemsetD32Async, hipError_t, (hipDeviceptr_t, int, size_t, hipStream_t)) \
  X(hipImportExternalMemory, hipError_t, (hipExternalMemory_t*, const hipExternalMemoryHandleDesc*)) \
  X(hipExternalMemoryGetMappedBuffer, hipError_t, (void**, hipExternalMemory_t, const hipExternalMemoryBufferDesc*)) \
  X(hipDestroyExternalMemory, hipError_t, (hipExternalMemory_t)) \
  X(hipImportExternalSemaphore, hipError_t, (hipExternalSemaphore_t*, const hipExternalSemaphoreHandleDesc*)) \
  X(hipSignalExternalSemaphoresAsync, hipError_t, (const hipExternalSemaphore_t*, const hipExternalSemaphoreSignalParams*, unsigned, hipStream_t)) \
  X(hipWaitExternalSemaphoresAsync, hipError_t, (const hipExternalSemaphore_t*, const hipExternalSemaphoreWaitParams*, unsigned, hipStream_t)) \
  X(hipDestroyExternalSemaphore, hipError_t, (hipExternalSemaphore_t))

struct HipApi {
  void* module = nullptr;
#define OPTINR_DECL(name, ret, args) ret(__cdecl* name) args = nullptr;
  OPTINR_HIP_FUNCS(OPTINR_DECL)
#undef OPTINR_DECL
  const char* err(hipError_t e) const { return hipGetErrorString ? hipGetErrorString(e) : "?"; }
};

// Loads the runtime and resolves every symbol; returns false with a reason on failure.
bool load_hip(HipApi& api, const wchar_t* dll, char* why, size_t why_len);
