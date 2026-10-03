# 构建与本地检查

本机开发依赖均保存在 `.tools` 内，没有安装驱动或修改 Apex。当前工具链：.NET SDK 10.0.401、MSVC 14.44.35207、Windows SDK 10.0.26100.0、FFmpeg 8.1 LGPL shared。

PowerShell 7 中运行：

```powershell
./scripts/build.ps1
./tests/ipc-smoke.ps1
./tests/settings-smoke.ps1
./tests/startup-smoke.ps1
./tests/video-smoke.ps1
```

构建包含规则、人声平衡和原生缓存/音频测试，并发布自包含程序到 `dist/Apex回放`。人声平衡测试 `tests/speech_tests.cpp` 只依赖标准 C++20，也可以在其他平台直接编译运行（如 `g++ -std=c++20 -O2 tests/speech_tests.cpp`）。重新构建前应退出运行中的应用和工作进程，以免 Windows 锁住需要替换的 EXE/DLL。

运行中的版本需要保留时，可使用 `./scripts/build.ps1 -OutputDirectory './artifacts/new-build'` 在独立目录构建和测试。设置回归测试使用隔离的设置文件，验证修改后立刻退出、再次启动、麦克风暂未连接时的配置恢复。

目录要求：`.tools/dotnet/dotnet.exe`；`.tools/msvc/setup_x64.bat`；`.tools/ffmpeg-extract/<build>/{bin,include,lib}`。完整运行包不需要这些开发工具。若从源代码在另一台电脑构建，准备同版本工具链或修改构建脚本中的工具路径。

依赖来源：

- [.NET 10 官方版本元数据](https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json)，SDK 压缩包已核对 SHA-512。
- [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/) 和微软 MSVC 工具链；本机使用 [portable-msvc 下载脚本](https://gist.github.com/mmozeiko/7f3162ec2988e81e56d5c4e22cde9977)，从微软源下载开发组件。
- [BtbN FFmpeg LGPL shared 构建](https://github.com/BtbN/FFmpeg-Builds/releases)，使用 `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`。
- [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0)。

## 原生诊断

```powershell
./dist/Apex回放/ApexPerif.Worker.exe --diagnose
./dist/Apex回放/ApexPerif.Worker.exe --analyze 'C:\Videos\reference.mp4' 'artifacts/test.jsonl' 120 30
./dist/Apex回放/ApexPerif.Worker.exe --inspect-image 'artifacts/frame.png'
./dist/Apex回放/ApexPerif.Worker.exe --inspect-image 'artifacts/frame.png' --gpu
```

`--analyze` 参数为录像、输出 JSONL、起点秒数和时长秒数；另输出 `.clips.json`。离线与实时共用 HUD 检测器、观察到规则的适配器和合并引擎。离线命令产出诊断及片段计划，未改变参考录像。

`--inspect-image --gpu` 另检查 1080p、1440p、4K 的 GPU 视频缩放和独立 HUD 分析表面，比较弹药、累计伤害和战斗状态。`--session-selftest` 可额外传入设置 JSON 路径，以验证选定的分辨率、编码器、目标码率和预设。视频回归脚本检查真实 MP4 的分辨率、60 fps、三个音轨、无黄色边框及录制时切换麦克风降噪。

```powershell
./dist/Apex回放/ApexPerif.Worker.exe --selftest 'artifacts/new-capture-test'
./dist/Apex回放/ApexPerif.Worker.exe --session-selftest 'artifacts/new-session-test'
./dist/Apex回放/Apex回放.exe --ui-smoke "$PWD/artifacts/ui.png"
```

`--ui-smoke` 输出概览页、设置页顶部、保存标准和通用设置的深浅两种截图：`ui.png`、`ui.settings.png`、`ui.criteria.png`、`ui.general.png` 及对应 `ui.light.*.png`。

`tests/startup-smoke.ps1` 使用隔离的配置及素材目录，验证启动界面后无需点击按钮就开始等待 Apex。`--startup` 用于 Windows 登录后的托盘启动；`--enable-startup` / `--disable-startup` 用于本地更新时设置当前运行包的自启注册；`--quit` 正常退出当前实例。普通启动和登录启动共享单实例限制，诊断 smoke 模式不占用正常实例。

采集测试会显示独立测试窗口，采集默认麦克风，并播放很轻的测试音。使用一个新的目录，以保留旧测试结果并避免覆盖文件。进程音轨排除测试可从另一个 PowerShell 进程启动 `--test-audio 880 12`，再运行 `--selftest`，两个音源应分属第一、第三轨。

`scripts/build-hud-templates.py` 是字形模板的校准记录，需要本机 `artifacts` 内对应的人工标注裁剪图和 Python 的 Pillow/numpy；普通运行和构建不依赖 Python。

## 使用的系统接口

- [Windows Graphics Capture 窗口接口](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)
- [Windows 进程音频采集样例](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- [Windows 采集边框控制](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired)
- [FFmpeg 麦克风 FFT 降噪](https://ffmpeg.org/ffmpeg-filters.html#afftdn)
- [Windows 本地 OCR](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrengine)
- [FFmpeg](https://ffmpeg.org/) 编解码及 MP4 封装
