# WiFi 提醒 / WiFi Warning

Language: [简体中文](#zh-cn) | [English](#english)

<a id="zh-cn"></a>

## 简体中文

WiFi 提醒是一款 Windows 桌面托盘工具。你可以按网络设置规则：连接到某个 WiFi 或有线网络时，点击被管控的快捷方式会先打开提醒页，而不是立刻启动软件。

它适合用来管住容易分心或需要被管控的软件，比如游戏、视频、聊天工具、网络代理工具。规则只改快捷方式，不改软件本体；快捷方式的名称、图标和位置保持不变。

[设置页截图]
<img width="2559" height="1131" alt="QQ20260610-130309" src="https://github.com/user-attachments/assets/54949d3d-a532-4c95-80c7-c1d9c1f854dc" />


### 功能

- 支持 WiFi 和有线网络规则。
- 有线网络支持一键断开和一键恢复。
- 软件组单独管理，先建组，再把要管控的 `.lnk` 快捷方式放进去。
- 替换快捷方式后，名称、图标和位置保持原样。
- 提醒页支持切换安全 WiFi、断开有线网络、输入密码允许本次启动。
- 中英文界面切换：点击 **简体中文** 或 **English** 即可切换。
- 绿色便携包：解压即用，不需要安装 Node.js、Python、CMake 或其他开发环境。
- 本地运行：设置页只在 `localhost` 打开，配置和日志保存在当前 Windows 用户目录，快捷方式备份保存在软件本体所在目录。

### 下载

下载绿色便携包：

[最新版 WiFiWarning-portable.zip](https://github.com/SSRYLJRSS/wifi-warning/releases/latest)

仓库里也会保留一份同名包：

[release/WiFiWarning-portable.zip](release/WiFiWarning-portable.zip)

普通使用只需要下载 zip 并解压，然后运行：

```powershell
.\wifi-warning.exe
```

后台启动：

```powershell
.\start-minimized.cmd
```

删除整个文件夹前，建议先还原快捷方式：

```powershell
.\restore-shortcuts.cmd
```


### 使用教程

#### 1. 打开设置

运行 `wifi-warning.exe`。程序会出现在系统托盘，并打开本地设置页。

如果浏览器没有自动打开，可以手动访问：

```text
http://localhost:18765/settings
```

#### 2. 创建软件组

在左侧点击 **软件组**。

点击 **新建软件组**，填写名称，然后点 **选择快捷方式**。这一步会调用 Windows 文件选择器，只需要选中想管控的 `.lnk` 快捷方式。

程序会记录这些快捷方式原来指向哪个软件。规则生效后，桌面或开始菜单里的快捷方式看起来不变，只是点击后会先经过 WiFi 提醒。

#### 3. 创建网络规则

回到 **网络规则**。

按页面上的三步来：

1. 选择需要管控的网络，可以是 WiFi，也可以是有线网卡。
2. 选择黑名单软件组。
3. 确定规则，让程序在后台运行。

#### 4. 触发提醒

当电脑连接到匹配的网络时，点击被管控的快捷方式会打开提醒页。

你可以选择：

- 不启动，直接返回。
- 切换到安全 WiFi。
- 有线网络下，一键断开有线连接。
- 输入密码，只允许这一次启动。

密码放行只对本次启动有效，不需要设置允许时长，也不会开启全局倒计时。

#### 5. 还原快捷方式

可以在设置页点击 **一键恢复快捷方式**，也可以运行：

```powershell
.\restore-shortcuts.cmd
```

### 从源码构建

只有开发、改代码或重新打包时才需要这些环境：

- Windows
- Git
- MinGW，或 CMake 加 C++20 编译器
- Node.js，用于前端语法检查和浏览器 smoke test

构建：

```powershell
.\scripts\build-mingw.ps1
```

打绿色包：

```powershell
.\scripts\package-portable.ps1
```

本地验收：

```powershell
.\scripts\local-acceptance.ps1
```

### 项目结构

```text
src/core/        配置、日志、网络检测、规则、快捷方式逻辑
src/ui/          本地 HTTP 服务、API、托盘图标
src/ww-launch/   被替换快捷方式调用的小启动器
frontend/        设置页、提醒页、网络选择页
scripts/         构建、打包、smoke test、验收脚本
tests/           原生 smoke test
docs/            截图和报告
release/         GitHub Release 使用的绿色包
```

### 验证状态

本地验证通过日期：2026-06-10。

- `build-mingw`
- `ww-smoke`
- `runtime-smoke`
- `tray-smoke`
- `browser-smoke`
- `validate-cmake`
- PowerShell parser checks
- `node --check`
- `package-portable`

运行时 smoke 测得工作集约 **4 MiB**。

### 许可证

本项目使用 [MIT License](LICENSE)。

你可以自由使用、复制、修改、二次开发、商业使用和再发布。再发布或商业使用时，请保留原始版权声明和 MIT 许可证文本，也就是标明代码来源。

返回语言切换：[English](#english)

<a id="english"></a>

## English

WiFi Warning is a Windows desktop tray app. You create rules by network: when the PC is connected to a chosen WiFi or wired network, opening a controlled shortcut shows a warning page before the real app starts.

It is built for simple self-control and managed-use cases around distracting or restricted apps, such as games, video apps, chat tools, or proxy tools. It changes shortcuts only, not the real apps. Shortcut names, icons, and locations stay the same.

[Settings screenshot]
<img width="2559" height="1131" alt="QQ20260610-130309" src="https://github.com/user-attachments/assets/54949d3d-a532-4c95-80c7-c1d9c1f854dc" />
### Features

- Rules for both WiFi and wired networks.
- One-click wired disconnect and restore.
- App groups are managed separately: create a group, then add `.lnk` shortcuts to it.
- Replaced shortcuts keep the same name, icon, and location.
- Warning page can switch to a safe WiFi, disconnect wired network, or allow this launch with a password.
- Chinese and English UI: click **简体中文** or **English** to switch.
- Portable package: extract and run. No Node.js, Python, CMake, or other development tools are needed for normal use.
- Local-first: settings run on `localhost`, config/logs stay under the current Windows user profile, and shortcut backups are saved alongside the executable.

### Download

Download the portable package:

[Latest WiFiWarning-portable.zip](https://github.com/SSRYLJRSS/wifi-warning/releases/latest)

The repository also keeps a copy:

[release/WiFiWarning-portable.zip](release/WiFiWarning-portable.zip)

For normal use, download the zip, extract it, then run:

```powershell
.\wifi-warning.exe
```

Start in the background:

```powershell
.\start-minimized.cmd
```

Before deleting the folder, restore shortcuts first:

```powershell
.\restore-shortcuts.cmd
```


### Tutorial

#### 1. Open Settings

Run `wifi-warning.exe`. The app appears in the system tray and opens a local settings page.

If the browser does not open automatically, visit:

```text
http://localhost:18765/settings
```

#### 2. Create an App Group

Click **App groups** in the left navigation.

Click **New app group**, name it, then click **Choose shortcuts**. Windows opens a file picker; select the `.lnk` shortcuts you want to control.

WiFi Warning records what each shortcut originally opened. When a rule is active, the shortcut still looks the same on the Desktop or Start Menu, but launch goes through WiFi Warning first.

#### 3. Create a Network Rule

Go back to **Network rules**.

Follow the three steps on the page:

1. Choose the network to control. It can be WiFi or a wired adapter.
2. Choose the blocked app group.
3. Save the rule and keep the tray app running.

#### 4. Trigger the Warning

When the PC is connected to the matching network, opening a controlled shortcut shows the warning page.

You can:

- stop and go back;
- switch to a safe WiFi;
- disconnect wired network;
- enter the password to bypass, which allows the app and blocks nothing for a configurable duration (default 30 minutes).

#### 5. Restore Shortcuts

Use **Restore all shortcuts** in settings, or run:

```powershell
.\restore-shortcuts.cmd
```

### Build From Source

These tools are only needed for development, code changes, or rebuilding the package:

- Windows
- Git
- MinGW, or CMake with a C++20 compiler
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
src/core/        Config, logging, network detection, rules, shortcut logic
src/ui/          Local HTTP server, API handlers, tray icon
src/ww-launch/   Small launcher used by replaced shortcuts
frontend/        Settings, warning page, network picker
scripts/         Build, package, smoke, acceptance scripts
tests/           Native smoke tests
docs/            Screenshots and reports
release/         Portable zip used for GitHub Release
```

### Validation

Local validation passed on 2026-06-10.

- `build-mingw`
- `ww-smoke`
- `runtime-smoke`
- `tray-smoke`
- `browser-smoke`
- `validate-cmake`
- PowerShell parser checks
- `node --check`
- `package-portable`

Runtime smoke measured about **4 MiB** working set.

### License

This project is released under the [MIT License](LICENSE).

You may use, copy, modify, fork, redistribute, and use it commercially. Redistribution or commercial use must keep the original copyright notice and MIT license text, which serves as source attribution.

Back to language switch: [简体中文](#zh-cn)
