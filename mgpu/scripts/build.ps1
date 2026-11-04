# Builds src/OptiScaler (Release x64) with VS 2022 (v143). Parallelism = logical threads - 2.
param([string]$Configuration = 'Release', [string]$Log)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
$n = [Math]::Max(1, [Environment]::ProcessorCount - 2)
$a = @((Join-Path $root 'src\OptiScaler.sln'), '/t:OptiScaler', "/p:Configuration=$Configuration", '/p:Platform=x64',
    "/m:$n", "/p:CL_MPCount=$n", '/nologo', '/v:m')
if ($Log) { $a += "/flp:LogFile=$Log;Verbosity=normal" }
& $msbuild @a
exit $LASTEXITCODE
