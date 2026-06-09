# WiFi 提醒 / WiFi Warning

语言 / Language: [简体中文](#zh-cn) | [English](#english)

<a id="zh-cn"></a>

## 简体中文

WiFi 提醒是一款 Windows 托盘小工具。你可以指定某些 WiFi 和某些软件快捷方式；当电脑连着这些 WiFi 时，点击被管控的快捷方式会先打开提醒页，而不是立刻启动软件。

它适合用来管住一些容易分心的软件。比如在学校、办公室或图书馆 WiFi 下，打开游戏、视频、聊天软件前，先让自己停一下。

![设置页截图](docs/screenshots/settings-desktop.png)

### 主要功能

- 按 WiFi 生效：只在你指定的 WiFi 下触发提醒。
- 按软件组管理：先建软件组，再把要管控的快捷方式放进组里。
- 不改软件本体：只替换 `.lnk` 快捷方式的启动目标。
- 外观不变：快捷方式的名称、图标、位置保持原样。
- 一键还原：可以把已替换的快捷方式恢复回原来的启动方式。
- 绿色便携：不用安装，解压后直接运行。
- 本地运行：设置页只在本机打开，配置和日志保存在当前 Windows 用户目录。

### 下载和运行

下载绿色便携包：

[下载最新版 WiFiWarning-portable.zip](https://github.com/SSRYLJRSS/wifi-warning/releases/latest)

仓库里也保留了一份同样的包：

[release/WiFiWarning-portable.zip](release/WiFiWarning-portable.zip)

普通使用不需要安装 Node.js、CMake、Python 或其他开发环境。下载 zip，解压，然后运行：

```powershell
.\wifi-warning.exe
```

如果想让它直接后台启动：

```powershell
.\start-minimized.cmd
```

删除整个文件夹前，建议先还原快捷方式：

```powershell
.\restore-shortcuts.cmd
```

### 截图

#### 设置页

![设置页截图](docs/screenshots/settings-desktop.png)

#### 提醒页

![提醒页截图](docs/screenshots/warning-desktop.png)

#### WiFi 选择页

![WiFi 选择页截图](docs/screenshots/wifi-picker-desktop.png)

### 使用教程

#### 1. 打开设置

运行 `wifi-warning.exe`。程序会出现在系统托盘，并打开本地设置页。

如果浏览器没有自动打开，可以手动访问：

```text
http://localhost:18765/settings
```

#### 2. 创建软件组

在左侧点击 **软件组**。

点击 **新建软件组**，填写名称，然后点 **选择快捷方式**。这一步会调用 Windows 文件选择器，你只需要选中想管控的 `.lnk` 快捷方式。

程序会记录这些快捷方式原来指向哪个软件。后面规则生效时，桌面或开始菜单里的快捷方式看起来不会变，只是点击后会先经过 WiFi 提醒。

#### 3. 创建 WiFi 规则

回到 **WiFi 规则**。

按页面上的三步来：

1. 选择需要管控的 WiFi。
2. 选择黑名单软件组。
3. 确定规则，并让程序在后台运行。

#### 4. 触发提醒

当电脑连接到匹配的 WiFi 时，点击被管控的快捷方式会打开提醒页。

你可以选择：

- 不启动，直接返回；
- 切换到其他 WiFi；
- 输入密码，只允许这一次启动。

密码放行只对本次启动有效，不会开启全局倒计时。

#### 5. 还原快捷方式

可以在设置页点 **一键恢复快捷方式**，也可以运行：

```powershell
.\restore-shortcuts.cmd
```

### 从源码构建

只有开发、改代码、重新打包时才需要这些环境：

- Windows
- Git
- CMake 和支持 C++17 的编译器，或 `PATH` 中可用的 MinGW
- Node.js，用于前端语法检查和浏览器烟测

构建：

```powershell
.\scripts\build-mingw.ps1
```

打包绿色包：

```powershell
.\scripts\package-portable.ps1
```

运行本地验收：

```powershell
.\scripts\local-acceptance.ps1
```

### 项目结构

```text
src/core/        配置、日志、WiFi、规则、快捷方式逻辑
src/ui/          本地 HTTP 服务、API、托盘图标
src/ww-launch/   被替换快捷方式调用的小启动器
frontend/        设置页、提醒页、WiFi 选择页
scripts/         构建、打包、烟测、验收脚本
tests/           原生烟测
docs/            报告和截图
release/         上传到 GitHub 的绿色包
```

### 验证状态

本地验证通过日期：2026-06-09。

- `build-mingw`
- `ww-smoke`
- `runtime-smoke`
- `tray-smoke`
- `browser-smoke`
- `validate-cmake`
- PowerShell parser checks
- Node `--check`
- `package-portable`

运行烟测测得工作集约约 **3.97 MiB**。

### 许可证

本项目使用 [MIT License](LICENSE)。

你可以自由使用、复制、修改、二次开发、商业使用和再发布。再发布或商业使用时，请保留原始版权声明和 MIT 许可证文本，也就是标明代码来源。

返回语言切换：[English](#english)

<a id="english"></a>

## English

WiFi Warning is a small Windows tray app. You choose WiFi networks and shortcut groups; when the PC is connected to a matching WiFi, opening a controlled shortcut shows a warning page before the real app starts.

It is meant for simple self-control around distracting apps. For example, on a school, office, or library WiFi, a game or video shortcut can ask you to pause before it opens.

![Settings screenshot](docs/screenshots/settings-desktop.png)

### Features

- WiFi-based rules: warnings only trigger on the networks you choose.
- Software groups: create a group first, then add shortcuts to it.
- No app modification: only `.lnk` shortcut targets are replaced.
- Same look: shortcut name, icon, and location stay the same.
- One-click restore: restore replaced shortcuts when you want.
- Portable package: no installer required.
- Local first: settings run on localhost, and data stays in the current Windows user profile.

### Download and Run

Download the portable package:

[Latest WiFiWarning-portable.zip](https://github.com/SSRYLJRSS/wifi-warning/releases/latest)

The same package is also kept in this repository:

[release/WiFiWarning-portable.zip](release/WiFiWarning-portable.zip)

Normal use does not require Node.js, CMake, Python, or any development tools. Download the zip, extract it, then run:

```powershell
.\wifi-warning.exe
```

To start it in the background:

```powershell
.\start-minimized.cmd
```

Before deleting the folder, restore shortcuts first:

```powershell
.\restore-shortcuts.cmd
```

### Screenshots

#### Settings

![Settings screenshot](docs/screenshots/settings-desktop.png)

#### Warning Page

![Warning screenshot](docs/screenshots/warning-desktop.png)

#### WiFi Picker

![WiFi picker screenshot](docs/screenshots/wifi-picker-desktop.png)

### Tutorial

#### 1. Open Settings

Run `wifi-warning.exe`. The app appears in the system tray and opens a local settings page.

If the browser does not open automatically, visit:

```text
http://localhost:18765/settings
```

#### 2. Create a Software Group

Click **Software Groups** in the left navigation.

Click **New Software Group**, name it, then click **Choose Shortcuts**. Windows will open a file picker; select the `.lnk` shortcuts you want to control.

WiFi Warning records what each shortcut originally opened. When a rule is active, the shortcut still looks the same on the Desktop or Start Menu, but launch goes through WiFi Warning first.

#### 3. Create a WiFi Rule

Go back to **WiFi Rules**.

Follow the three steps on the page:

1. Choose the WiFi network.
2. Choose the shortcut group.
3. Confirm the rule and keep the tray app running.

#### 4. Trigger the Warning

When the PC is connected to the matching WiFi, opening a controlled shortcut shows the warning page.

You can:

- stop and go back;
- switch to another WiFi;
- enter the password to allow only this launch.

Password bypass is one-time only. It does not start a global countdown.

#### 5. Restore Shortcuts

Use **One-click restore shortcuts** in settings, or run:

```powershell
.\restore-shortcuts.cmd
```

### Build From Source

These tools are only needed for development, code changes, or rebuilding the package:

- Windows
- Git
- CMake with a C++17-capable compiler, or MinGW available in `PATH`
- Node.js for frontend checks and browser smoke tests

Build:

```powershell
.\scripts\build-mingw.ps1
```

Package:

```powershell
.\scripts\package-portable.ps1
```

Run local acceptance:

```powershell
.\scripts\local-acceptance.ps1
```

### Project Layout

```text
src/core/        Config, logging, WiFi, rules, shortcut logic
src/ui/          Local HTTP server, API handlers, tray icon
src/ww-launch/   Small launcher used by replaced shortcuts
frontend/        Settings, warning page, WiFi picker
scripts/         Build, package, smoke, acceptance scripts
tests/           Native smoke tests
docs/            Reports and screenshots
release/         Portable zip uploaded to GitHub
```

### Validation

Local validation passed on 2026-06-09.

- `build-mingw`
- `ww-smoke`
- `runtime-smoke`
- `tray-smoke`
- `browser-smoke`
- `validate-cmake`
- PowerShell parser checks
- Node `--check`
- `package-portable`

Runtime smoke measured about **3.97 MiB** working set.

### License

This project is released under the [MIT License](LICENSE).

You may use, copy, modify, fork, redistribute, and use it commercially. Redistribution or commercial use must keep the original copyright notice and MIT license text, which serves as source attribution.

Back to language switch: [简体中文](#zh-cn)
