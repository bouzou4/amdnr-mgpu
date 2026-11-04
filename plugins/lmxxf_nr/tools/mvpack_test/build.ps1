# Builds mvpack_test.exe (VS 2022 x64 tools) into x64\mvpack_test\ at the repository root.
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$out = (New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot '..\..\..\..\x64\mvpack_test')).FullName
$a = Join-Path $PSScriptRoot 'main.cpp'
$b = Join-Path $PSScriptRoot '..\..\src\mgpu\MvPack.cpp'
& cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /O2 /EHsc /std:c++20 /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX `"$a`" `"$b`" /Fo`"$out/`" /Fe`"$out\mvpack_test.exe`""
exit $LASTEXITCODE
