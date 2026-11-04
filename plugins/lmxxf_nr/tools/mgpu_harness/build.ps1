# Builds mgpu_harness.exe (VS 2022 x64 tools) into x64\mgpu_harness\ at the repository root.
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$out = Join-Path $PSScriptRoot '..\..\..\..\x64\mgpu_harness'
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $PSScriptRoot 'main.cpp'
& cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /O2 /EHsc /std:c++20 /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX `"$src`" /Fo`"$out\\`" /Fe`"$out\mgpu_harness.exe`""
exit $LASTEXITCODE
