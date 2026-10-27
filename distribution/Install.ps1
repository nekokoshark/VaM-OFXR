param([Parameter(Mandatory=$true)][string]$VaMRoot)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $VaMRoot).Path
if (!(Test-Path -LiteralPath (Join-Path $root 'VaM.exe'))) { throw 'VaM.exe not found' }
if (Get-Process VaM -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $root 'VaM.exe') }) { throw 'Exit VaM before installing' }
$names = @('OFXR-Optimized','VaM (OFXR Standard Dynamic).bat','VaM (OFXR Standard 2x).bat')
foreach ($name in $names) {
    if (Test-Path -LiteralPath (Join-Path $root $name)) { throw "Already exists: $name. Uninstall this package first." }
}
foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $root -Recurse }
Write-Output 'INSTALL_PASS existing_runtime_untouched=1'
