# Builds lmxxf_nr.asi (Release x64, VS 2022 v143) into x64\lmxxf_nr\. Parallelism = logical threads - 2.
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
$n = [Math]::Max(1, [Environment]::ProcessorCount - 2)
& $msbuild (Join-Path $PSScriptRoot 'lmxxf_nr.vcxproj') /p:Configuration=Release /p:Platform=x64 "/m:$n" "/p:CL_MPCount=$n" /nologo /v:m
exit $LASTEXITCODE
