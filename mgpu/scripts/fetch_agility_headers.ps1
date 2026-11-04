# Vendors the DirectX 12 Agility SDK headers (MIT, LICENSE-CODE.txt) into third_party/directx_agility_sdk.
# They supply D3D12 declarations newer than Windows SDK 10.0.22621 (static samplers 1, root signature 1.2).
param(
    [string]$Version = '1.615.1',
    [string]$Sha256 = '20b67ae94eda93b9e1712362647d4c7b7d3b2d9150fded15950294d81611f17d'
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$dest = Join-Path $root 'third_party\directx_agility_sdk'
$tmp = Join-Path ([IO.Path]::GetTempPath()) "agility-$Version-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    $pkg = Join-Path $tmp 'd3d12.zip'
    Invoke-WebRequest -UseBasicParsing -OutFile $pkg "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$Version/microsoft.direct3d.d3d12.$Version.nupkg"
    $got = (Get-FileHash -Algorithm SHA256 $pkg).Hash.ToLower()
    if ($got -ne $Sha256) { throw "SHA256 mismatch: $got" }
    Expand-Archive -Force $pkg (Join-Path $tmp 'x')
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Force (Join-Path $dest 'include') | Out-Null
    $inc = Join-Path $tmp 'x\build\native\include'
    Copy-Item (Join-Path $inc '*.h'), (Join-Path $inc '*.hpp') (Join-Path $dest 'include')
    Copy-Item -Recurse (Join-Path $inc 'd3dx12') (Join-Path $dest 'include')
    Copy-Item (Join-Path $tmp 'x\LICENSE-CODE.txt') $dest
    Set-Content -Encoding ascii (Join-Path $dest 'VERSION.txt') "Microsoft.Direct3D.D3D12 $Version (nuget.org), sha256 $Sha256; headers only"
} finally { Remove-Item -Recurse -Force $tmp }
