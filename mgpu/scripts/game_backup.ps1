# Backs up every file of a game folder except its data packs, with a SHA-256 manifest.
# Run once before installing a test build; game_revert.ps1 restores the folder from it exactly.
param(
    [Parameter(Mandatory)] [string]$GameDir,
    [Parameter(Mandatory)] [string]$BackupDir,
    [string]$Exclude = '^data(_pack2)?\\'
)
$ErrorActionPreference = 'Stop'
$GameDir = (Resolve-Path $GameDir).Path.TrimEnd('\')
$manifest = Join-Path $BackupDir 'manifest.csv'
if (Test-Path $manifest) { throw "a backup already exists: $manifest" }
New-Item -ItemType Directory -Force (Join-Path $BackupDir 'files') | Out-Null

$rows = foreach ($f in Get-ChildItem $GameDir -Recurse -File) {
    $rel = $f.FullName.Substring($GameDir.Length + 1)
    if ($rel -match $Exclude) { continue }
    $dst = Join-Path $BackupDir "files\$rel"
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item -LiteralPath $f.FullName $dst
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $dst).Hash -ne $hash) { throw "copy mismatch: $rel" }
    (Get-Item -LiteralPath $dst).LastWriteTimeUtc = $f.LastWriteTimeUtc
    [pscustomobject]@{ Path = $rel; Length = $f.Length; LastWriteTimeUtc = $f.LastWriteTimeUtc.ToString('o'); SHA256 = $hash }
}
$rows | Export-Csv -NoTypeInformation $manifest
$dirs = Get-ChildItem $GameDir -Recurse -Directory | % { $_.FullName.Substring($GameDir.Length + 1) } | ? { $_ -notmatch $Exclude -and $_ -notmatch '^data(_pack2)?$' }
$dirs | Set-Content (Join-Path $BackupDir 'dirs.txt')
"{0} files backed up to {1}" -f @($rows).Count, $BackupDir
