param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
cmake -S "$PSScriptRoot/source" -B "$PSScriptRoot/build" -G "Visual Studio 17 2022" -A x64 -DXRFG_SERIALIZE_NVAPI_VRS=OFF -DXRFG_BUILD_STANDALONE=OFF
if ($LASTEXITCODE) { throw 'CMake configuration failed' }
cmake --build "$PSScriptRoot/build" --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
