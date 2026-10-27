@echo off
setlocal
set "XRFG_LATE60="
set "DISABLE_XR_APILAYER_DIENERTECH_VECTORXR=1"
set "XRFG_DISABLE_OFXR_BRIDGE="
set "VECTORXR_CONFIG_PATH="
set "VECTORXR_LOG_PATH="
set "VECTORXR_SEEN_APPS_PATH="
set "VAM_ENABLE_OFXR=1"
set "XR_API_LAYER_PATH=%~dp0OFXR-Optimized\manifests"
set "XR_ENABLE_API_LAYERS=XR_APILAYER_XRFrameBridge_diagnostic"
if not exist "%~dp0VaM.exe" exit /b 2
if not exist "%XR_API_LAYER_PATH%\OFXR_standard.json" exit /b 3
cd /d "%~dp0"
start "VaM" "%~dp0VaM.exe" -vrmode OpenVR
exit /b 0
