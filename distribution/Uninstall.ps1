param([Parameter(Mandatory=$true)][string]$VaMRoot)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $VaMRoot).Path
if (!(Test-Path -LiteralPath (Join-Path $root 'VaM.exe'))) { throw 'VaM.exe not found' }
if (Get-Process VaM -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $root 'VaM.exe') }) { throw 'Exit VaM before uninstalling' }
$folder = Join-Path $root 'OFXR-Optimized'
if (Test-Path -LiteralPath $folder) {
    $resolved = (Resolve-Path -LiteralPath $folder).Path
    if ($resolved -ne $folder -or (Get-Item -LiteralPath $folder).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unexpected install target' }
    Remove-Item -LiteralPath $folder -Recurse -Force
}
foreach ($name in @('VaM (OFXR Standard Dynamic).bat','VaM (OFXR Standard 2x).bat')) {
    $path = Join-Path $root $name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}
Write-Output 'UNINSTALL_PASS existing_runtime_untouched=1'
