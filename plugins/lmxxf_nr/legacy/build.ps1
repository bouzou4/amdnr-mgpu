# Builds the legacy-mode set into x64\lmxxf_legacy\ at the repository root: d3d9.dll (x86 game proxy),
# lmxxf_legacy_host.exe (x64 NR host), lmxxf_legacy_test.exe (x86 test title). Parallelism = logical threads - 2.
param([string]$Out = 'lmxxf_legacy') # output folder under x64\ (another one while a play build runs)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$out = Join-Path $root "x64\$Out"
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force "$obj\x86", "$obj\x64" | Out-Null
$n = [Math]::Max(1, [Environment]::ProcessorCount - 2)
$src = Join-Path $PSScriptRoot '..\src'
$common = "/nologo /O2 /MT /EHsc /std:c++20 /W3 /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /MP$n"

# Menu: Dear ImGui as OptiScaler vendors it (src/OptiScaler/include/imgui), stb_truetype instead of FreeType
$im = "$root\src\OptiScaler\include\imgui"
$imSrc = ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'imgui_impl_dx12.cpp' | ForEach-Object { "`"$im\$_`"" }) -join ' '
$proxy = "cl $common /LD /I`"$im`" /FI`"$PSScriptRoot\proxy\ImguiForced.h`" /Fo`"$obj\x86\\`" /Fe`"$out\d3d9.dll`" `"$PSScriptRoot\proxy\Proxy.cpp`" `"$PSScriptRoot\proxy\Overlay.cpp`" $imSrc /link /DEF:`"$PSScriptRoot\proxy\d3d9.def`" d3d12.lib dxgi.lib dxguid.lib user32.lib d3dcompiler.lib"
$test = "cl $common /Fo`"$obj\x86\\`" /Fe`"$out\lmxxf_legacy_test.exe`" `"$PSScriptRoot\test\LegacyTest.cpp`" /link /SUBSYSTEM:CONSOLE user32.lib"
$hostCmd = "cl $common /I`"$root\src\OptiScaler\include`" /Fo`"$obj\x64\\`" /Fe`"$out\lmxxf_legacy_host.exe`" `"$PSScriptRoot\host\Host.cpp`" `"$PSScriptRoot\host\OpticalFlow.cpp`" `"$PSScriptRoot\host\SelfTest.cpp`" `"$src\Nr.cpp`" `"$src\Runtime.cpp`" `"$src\Autoconfig.cpp`" `"$src\mgpu\MgpuDump.cpp`" /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib dxguid.lib gdi32.lib bcrypt.lib"

cmd /c "`"$vcvars`" x86 >nul && $proxy && $test"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmd /c "`"$vcvars`" x64 >nul && $hostCmd"
exit $LASTEXITCODE
