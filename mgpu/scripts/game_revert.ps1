# Restores a game folder to a game_backup.ps1 backup: every manifest file byte-exact (SHA-256),
# files and folders the backup does not know removed. Data packs are never touched. Dry run unless -Apply.
param(
    [Parameter(Mandatory)] [string]$GameDir,
    [Parameter(Mandatory)] [string]$BackupDir,
    [string]$Exclude = '^data(_pack2)?\\',
    [switch]$Apply
)
$ErrorActionPreference = 'Stop'
$GameDir = (Resolve-Path $GameDir).Path.TrimEnd('\')
$rows = Import-Csv (Join-Path $BackupDir 'manifest.csv')
$exe = Get-Process -ErrorAction SilentlyContinue | ? { $_.Path -and $_.Path.StartsWith($GameDir, 'OrdinalIgnoreCase') }
if ($exe) { throw "the game is running: $($exe.Name -join ', ')" }

$known = @{}
foreach ($r in $rows) {
    $known[$r.Path.ToLowerInvariant()] = $true
    $dst = Join-Path $GameDir $r.Path
    $ok = (Test-Path -LiteralPath $dst) -and (Get-FileHash -Algorithm SHA256 -LiteralPath $dst).Hash -eq $r.SHA256
    if ($ok) { continue }
    "restore $($r.Path)"
    if (-not $Apply) { continue }
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item -Force -LiteralPath (Join-Path $BackupDir "files\$($r.Path)") $dst
    (Get-Item -LiteralPath $dst).LastWriteTimeUtc = [datetime]::Parse($r.LastWriteTimeUtc).ToUniversalTime()
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $dst).Hash -ne $r.SHA256) { throw "restore mismatch: $($r.Path)" }
}
foreach ($f in Get-ChildItem $GameDir -Recurse -File) {
    $rel = $f.FullName.Substring($GameDir.Length + 1)
    if ($rel -match $Exclude -or $known.ContainsKey($rel.ToLowerInvariant())) { continue }
    "remove $rel"
    if ($Apply) { Remove-Item -LiteralPath $f.FullName }
}
$dirs = @(Get-Content (Join-Path $BackupDir 'dirs.txt') | % { $_.ToLowerInvariant() })
Get-ChildItem $GameDir -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending | % {
    $rel = $_.FullName.Substring($GameDir.Length + 1)
    if ($rel -match '^data(_pack2)?($|\\)' -or $dirs -contains $rel.ToLowerInvariant()) { return }
    if (-not (Get-ChildItem -LiteralPath $_.FullName -Force)) { "remove dir $rel"; if ($Apply) { Remove-Item -LiteralPath $_.FullName } }
}
if (-not $Apply) { 'dry run; pass -Apply to change the folder' } else { 'reverted' }
