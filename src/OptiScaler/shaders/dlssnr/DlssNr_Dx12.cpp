// AMDNR gap replacement (docs/AMDNR_GAPS.md). AMDNR's own DlssNr_Dx12.cpp (the NVIDIA nvngx_dlssnr
// path and the evaluate dispatcher) is not in its published source. This keeps the namespace API of
// dlssnr/DlssNrFeature_Dx12.h and DlssNrFeature_Vk.h and routes the AMD path to DlssNr::AmdBridge.
// The NVIDIA model path and the native Vulkan pass are off.
#include "pch.h"

#include <Config.h>

#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNrFeature_Vk.h>
#include <dlssnr/amd/AmdBridge.h>
#include <dlssnr/gi/GiSeam.h>

namespace DlssNr
{
namespace
{
bool Enabled() { return Config::Instance()->DlssNrEnabled.value_or_default(); }
bool RunBeforeSr() { return Config::Instance()->DlssNrRunBeforeSr.value_or_default(); }
} // namespace

void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue, unsigned long long submissionEpoch)
{
    if (cmdList == nullptr || params == nullptr)
        return;

    // GI sits before the NR gate, as AMDNR's GiSeam.h describes; with [AmdGi] off it does nothing.
    if (AmdnrGi::Seam::Wanted())
        AmdnrGi::Seam::BeforeNr(cmdList, params, timingQueue, submissionEpoch);

    if (Enabled() && RunBeforeSr())
        AmdBridge::Before(cmdList, params, timingQueue, submissionEpoch);
}

void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue, bool forcePost, unsigned long long submissionEpoch)
{
    if (cmdList == nullptr || params == nullptr || !Enabled())
        return;

    // Post placement: after SR when pre-SR is off, after RR when ApplyAfterRR asks for it.
    const bool post = forcePost ? Config::Instance()->DlssNrApplyAfterRR.value_or_default() : !RunBeforeSr();
    if (post)
        AmdBridge::After(cmdList, params, timingQueue, submissionEpoch);
}

void RetryAfterFailure() {}
void ProbeD3D11(void*) {}
CalibrationReading Calibration() { return {}; }
bool IsRunning() { return AmdBridge::BackendActive(); }
const char* FailureReason() { return ""; }
ExposureStatus GameExposureStatus() { return {}; }
std::optional<double> LastGpuTime() { return std::nullopt; }
void RequestCapture(unsigned int frames) { AmdBridge::RequestCapture(0); }
bool CaptureInProgress() { return false; }
void Shutdown() {}

void EvaluateAfterUpscaleVk(VkCommandBuffer, NVSDK_NGX_Parameter*, VkInstance, VkPhysicalDevice, VkDevice) {}
bool IsRunningVk() { return false; }
const char* FailureReasonVk() { return "the native Vulkan pass is not in AMDNR's published source"; }
unsigned long long FramesVk() { return 0; }
std::optional<double> LastGpuTimeVk() { return std::nullopt; }
bool ExposureOfferedVk() { return false; }
void ShutdownVk(bool) {}
} // namespace DlssNr
