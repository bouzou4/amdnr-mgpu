// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// What the multi-GPU modules take from the platform (they know nothing of OptiScaler or the plugin API).
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <winternl.h> // NTSTATUS for d3dkmthk.h

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
