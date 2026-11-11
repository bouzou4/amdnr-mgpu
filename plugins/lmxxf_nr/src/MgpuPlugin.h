// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
#pragma once
#include <string>

#include "../../../src/plugin_sdk/optiscaler_plugin.h"

class NrEngine;

// From OptiPluginInit: reads the MultiGpu keys and registers the adapter override and swapchain provider
bool MgpuInit(const OptiPluginApi* api, OptiPlugin* plugin, const std::wstring& dumpDir);
bool MgpuActive(); // the game renders on the render card: no NR there
bool MgpuFallback(); // the display card was lost: single-GPU passthrough, NR off
bool MgpuWantsHudless();
uint32_t MgpuPresentRequests(); // OptiPresentRequest bits multi-GPU needs
void MgpuOnPresent(OptiPresentFrame* frame);
void MgpuOnUpscale(OptiUpscaleFrame* frame);
void MgpuDrawTab(const OptiPluginApi* api);
bool MgpuOverlayText(char* buf, size_t size);
bool MgpuOverlayHint(char* buf, size_t size); // links and what limits multi-GPU
void MgpuEnsureRuntime(NrEngine& e);
void MgpuLoadRuntime(NrEngine& e); // Plugin.cpp: the runtime search folders
