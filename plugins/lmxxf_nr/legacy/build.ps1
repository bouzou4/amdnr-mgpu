# Builds the legacy-mode set into x64\lmxxf_legacy\ at the repository root: d3d9.dll (x86 game proxy),
# lmxxf_legacy_host.exe (x64 NR host), lmxxf_legacy_test.exe (x86 test title). Parallelism = logical threads - 2.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$out = Join-Path $root 'x64\lmxxf_legacy'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force "$obj\x86", "$obj\x64" | Out-Null
$n = [Math]::Max(1, [Environment]::ProcessorCount - 2)
$src = Join-Path $PSScriptRoot '..\src'
$common = "/nologo /O2 /MT /EHsc /std:c++20 /W3 /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /MP$n"

$proxy = "cl $common /LD /Fo`"$obj\x86\\`" /Fe`"$out\d3d9.dll`" `"$PSScriptRoot\proxy\Proxy.cpp`" /link /DEF:`"$PSScriptRoot\proxy\d3d9.def`" d3d12.lib dxgi.lib dxguid.lib"
$test = "cl $common /Fo`"$obj\x86\\`" /Fe`"$out\lmxxf_legacy_test.exe`" `"$PSScriptRoot\test\LegacyTest.cpp`" /link /SUBSYSTEM:CONSOLE user32.lib"
$hostCmd = "cl $common /I`"$root\src\OptiScaler\include`" /Fo`"$obj\x64\\`" /Fe`"$out\lmxxf_legacy_host.exe`" `"$PSScriptRoot\host\Host.cpp`" `"$PSScriptRoot\host\OpticalFlow.cpp`" `"$src\Nr.cpp`" `"$src\Runtime.cpp`" `"$src\mgpu\MgpuDump.cpp`" /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib dxguid.lib"

cmd /c "`"$vcvars`" x86 >nul && $proxy && $test"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmd /c "`"$vcvars`" x64 >nul && $hostCmd"
exit $LASTEXITCODE
