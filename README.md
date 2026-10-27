# VaM-OFXR：标准版 2× 光流补帧＋动态迟到补位

这是基于 [OFXR-Bridge](https://github.com/tig3rmast3r/OFXR-Bridge) V068 / v0.2.0 修改的社区测试版，不是上游官方发行版。

## 功能与边界
- 普通 2×入口，以及约 60Hz 输出节拍的动态补位入口。不是严格锁 60，也不保证原始帧率。
- 真实帧迟到时复用同一真实帧对的光流，最多五个预测时隙、83.33ms且不超过一段源帧间隔；不递归使用生成帧。
- GPU忙、真实帧到达、双眼任一失败或预算超限时退让，限制追赶和等待。
- 保留 D3D11 多线程设备/上下文同步，右上角纯数字 FPS。
- **此标准包不需要注视点渲染，也不含 dxgi.dll、FFR配置或 NVAPI VRS hook。使用自定义FFR链的用户不要用它覆盖已验证的FFR兼容版。**
- 私有runtime/真实NVIDIA GPU的双眼、ownership、超时、普通模式回归通过；该标准包尚未完成实际头显视觉、延迟及VR→VD切换验收。低原生帧率下预测仍可能有伪影。

## 必要环境及依赖
Windows 10/11 x64；支持 NVIDIA Optical Flow 的NVIDIA显卡及驱动（OFA支持通常始于Turing，具体以驱动/API为准）；可工作的OpenXR runtime（例如VDXR）。

VaM使用OpenVR，因此还需要OpenComposite OpenXR转接。已有可工作的转接时不必替换。包内附微软签名的 `dependencies/vc_redist.x64.exe`（Microsoft Visual C++ 2015–2022 x64运行库，按需手动安装）；NVIDIA光流运行库来自显卡驱动，不分发系统DLL或完整NVIDIA SDK。

可选：正常退出VaM后执行 `Get-OpenComposite.ps1 -VaMRoot "D:\VaM"` 从[官方地址](https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr)下载并安装x64 OpenComposite；它保留原openvr_api.dll，使用 `-Restore` 还原。这是联网获取当前官方版本，不是固定版本依赖；来源/源码：[OpenOVR openxr分支](https://gitlab.com/znixian/OpenOVR/-/tree/openxr)，GPLv3。不会替换本包以外的DLL，除非你主动执行此可选脚本。

## 安装、启动、还原
1. 从Release下载运行ZIP，解压到临时目录。正常退出VaM。
2. PowerShell执行 `./Install.ps1 -VaMRoot "D:\VaM"`。不修改全局OpenXR运行时/注册表，不覆盖原启动入口、原OFXR或FFR。
3. 先连接头显并保持唤醒，使用新入口 `VaM (OFXR Standard 2x).bat` 或 `VaM (OFXR Standard Dynamic).bat`。SSW设置由用户选择，脚本不修改。
4. 还原：退出VaM后运行 `./Uninstall.ps1 -VaMRoot "D:\VaM"`；然后使用原启动入口。若主动换过OpenComposite，另用 `Get-OpenComposite.ps1 -VaMRoot "D:\VaM" -Restore`。

运行包不含VaM资源、场景、个人配置、日志或转储。

## 构建
安装VS2022 C++桌面工具、Windows SDK（含fxc）和CMake>=3.24，在ASCII路径检出此仓库，执行 `./Build.ps1`。
源码包含最小OpenXR/NVIDIA接口头、MinHook、OpenVR头以及AMD MIT许可的头文件和两项静态lib。AMD来源固定为FidelityFX SDK v1.1.4提交 `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`；没有AMD运行DLL依赖。
默认源码选项 `XRFG_SERIALIZE_NVAPI_VRS=ON` 保留兼容分支；`Build.ps1`明确设OFF生成本标准版。开启该选项本身不代表任意FFR组合已验证。

定向回归：`ctest --test-dir build -C Release --output-on-failure -E "3x|d3d12_history_tests"`，以及build/Release中的dynamic_cover_policy、dynamic_cover_gpu和late60_layer夹具。该发行版重点验证NVIDIA 2×，不宣称3×通过。

## 上游与许可
保留上游随V068提供的LGPLv3 `LICENSE`、GPLv3全文及各组件许可，不以新的MIT许可覆盖原许可。详见 `THIRD_PARTY.md`、`licenses/` 和 `source/external/`。修改后的完整对应源码随仓库和源码ZIP提供。MinHook和OpenVR分别保留其原许可。VC运行库是微软官方可再分发安装器，其许可在安装器中。
