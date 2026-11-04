cmake_minimum_required(VERSION 3.25)
project(amdnr_mgpu_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(NOT WIN32)
  message(FATAL_ERROR "Windows (D3D12/DXGI) only")
endif()
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

add_executable(optinr_diag
  optinr_diag/main.cpp
  optinr_diag/pcie.cpp
  optinr_diag/hip_interop.cpp
  optinr_diag/xadapter.cpp)
target_compile_definitions(optinr_diag PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(optinr_diag PRIVATE d3d12 dxgi gdi32 setupapi cfgmgr32)
if(MSVC)
  target_compile_options(optinr_diag PRIVATE /W4 /permissive- /utf-8)
endif()

add_subdirectory(gpu_pref_probe)

# sl_tap needs the Streamline and FFX API headers: scripts/fetch_streamline_headers.ps1, scripts/fetch_ffx_headers.ps1.
if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/streamline/include/sl.h AND EXISTS ${PROJECT_SOURCE_DIR}/third_party/ffx/Kits/FidelityFX/api/include/ffx_api.h)
  add_subdirectory(sl_tap)
else()
  message(STATUS "sl_tap skipped: headers not fetched")
endif()
