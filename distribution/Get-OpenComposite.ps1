param([Parameter(Mandatory=$true)][string]$VaMRoot, [switch]$Restore)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $VaMRoot).Path
if (!(Test-Path -LiteralPath (Join-Path $root 'VaM.exe'))) { throw 'VaM.exe not found' }
if (Get-Process VaM -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $root 'VaM.exe') }) { throw 'Exit VaM first' }
$target = Join-Path $root 'VaM_Data/Plugins/openvr_api.dll'
$backup = "$target.before-ofxr-standard"
if ($Restore) {
    if (!(Test-Path -LiteralPath $backup)) { throw 'Backup not found' }
    Move-Item -LiteralPath $backup -Destination $target -Force
    Write-Output 'OPENCOMPOSITE_RESTORED'
    exit 0
}
if (Test-Path -LiteralPath $backup) { throw 'Backup already exists; restore first' }
$download = "$target.ofxr-download"
try {
    Invoke-WebRequest 'https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr' -OutFile $download -UseBasicParsing
    $bytes = [IO.File]::ReadAllBytes($download)
    if ($bytes.Length -lt 1024 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { throw 'Invalid DLL download' }
    Copy-Item -LiteralPath $target -Destination $backup
    Move-Item -LiteralPath $download -Destination $target -Force
    Get-FileHash -LiteralPath $target -Algorithm SHA256
} finally { if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download } }
