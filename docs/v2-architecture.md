# WiFiWarning v2.0 — 架构与重构方案

> 目标：Windows 11 专用、运行稳定、占用低、界面美观的网络监控与拦截工具。修复“拦截经常失效”与“定位服务误提示”两大根因。

## 1. 语言 / 框架选型（决策矩阵）

| 方案 | 稳定 | 占用 | 仅Win11适配 | 界面美观 | 结论 |
|---|---|---|---|---|---|
| **C++20 / Win32 原生核心 + 本地静态 Web UI** | ★★★★★ | ★★★★★（工作集 ~4MiB） | ★★★★★ | ★★★★（Web UI 可做精美） | **选用** |
| C# / .NET 10 + WinUI3/WPF | ★★★★ | ★★（自包含 80–150MB；框架依赖需装运行时） | ★★★★ | ★★★★★（原生 Mica/亚克力） | 占用不达标 |
| Rust + Tauri | ★★★★ | ★★★（WebView2 依赖） | ★★★ | ★★★★ | 生态/托盘复杂 |
| Electron | ★★★ | ★（~150MB+） | ★★★ | ★★★★ | 占用严重超标 |

**决策：保留并重构 C++20/Win32 原生核心（低占用、高稳定、原生托盘、可直接操作网络），完全重建前端为现代静态 Web UI。** 这是唯一同时满足“稳定 + 占用少”的选择；用户可见的“美观/操作逻辑”全部由重建的前端承担。

## 2. 现有架构（v1.9.x）

- `src/core/`：config_manager、logger、network_manager、rule_engine、shortcut_manager、wifi_detector、wifi_switcher、http_probe、dyn_wlan、json、util、auto_start
- `src/ui/`：http_server（localhost:18765）、api_handlers、tray_icon、browser_launcher
- `src/ww-launch/`：被 .lnk 替换后调用的小启动器
- `frontend/`：settings.html / warning.html / wifi_picker.html + 原生 JS（多页跳转）

## 3. 根因分析

### 3.1 “定位服务一直提示”
- `src/ui/tray_icon.cpp:383-389`：后台线程 **长期持有 WlanOpenHandle 并 `WlanRegisterNotification(ACM)`**（`tray_icon.cpp:45-57`），整个 app 生命周期持续订阅 → Windows 持续显示“定位使用中”。
- `src/core/wifi_detector.cpp:100-113`：每 30s 调用 `WlanQueryInterface(wlan_intf_opcode_current_connection)` 读取 SSID/BSSID，属定位敏感数据。
- `src/core/wifi_switcher.cpp:68`：`WlanGetAvailableNetworkList`（扫描，强定位）。
- 结论：即使动态加载 DLL、去掉 manifest 能力，**运行期仍调用定位敏感 WLAN API 且长期持有句柄**，故指示器常驻。

### 3.2 “拦截经常失效”
- 拦截完全依赖 **重写 .lnk 快捷方式**（`shortcut_manager.cpp:234-268`）→ 直接双击 exe、任务栏/UWP、更新器/启动器（Steam 等）、App Paths、Run 对话框均绕过。
- `shortcut_manager.cpp:169-192` `findShortcutsForApp` 要求 target 路径 **精确相等**；指向启动器/带参数/版本化路径/大小写差异的快捷方式匹配不到 → 不替换 → 不拦截。
- `rule_engine.cpp:25-46` 以 `original_path == appPath` 精确匹配；`ww-launch` 传入路径与记录不符即 **fail-open 直接启动真实程序**（无提示）。
- 30s TTL 缓存（wifi_detector / network_manager）：刚切到受控网络后立即启动，缓存仍旧 → 规则不命中 → 放行。
- app 更新器重建快捷方式后 `reconcileShortcutRecords`(shortcut_manager.cpp:366) 静默丢弃记录。
- 结论：**只看快捷方式 + fail-open + 缓存过期** 三重叠加 = “经常拦截不了”。

### 3.3 “操作逻辑怪 / 页面翻来翻去”
- 多页跳转（settings/warning/wifi_picker 各自 HTML），导航为左侧按钮切 `.panel`；每次操作 wired 开关走 `netsh runas`（network_manager.cpp:443-463）→ **每次都弹 UAC**，体验割裂。UI 无统一状态栏，信息分散。

## 4. v2 架构

### 4.1 网络引擎（无定位）
- 连通状态：**INetworkListManager (COM) + GetAdaptersAddresses**（不触发定位）。
- 实时：NLM `INetworkListManager` 事件通知（`Advise`），替代 WlanRegisterNotification；**不常驻 WLAN 句柄**。
- SSID：仅在打开面板/手动刷新时按需读取一次（`WlanQueryInterface current_connection`），后台不轮询。新增设置“隐藏 SSID，显示配置名”以彻底规避。
- 明确区分 WiFi 与 以太网 适配器（`IF_TYPE_IEEE80211` vs `IF_TYPE_ETHERNET_CSMACD`，排除虚拟/蓝牙）。

### 4.2 拦截（可靠、无驱动）
- **主方案：IFEO 映像劫持** —— 对目标 exe 写 `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<exe>\Debugger = "<install>\ww-launch.exe" --real-launch`。任何方式启动该 exe（双击/快捷方式/任务栏/更新器/Run）都先经 ww-launch。**不改快捷方式**（名称/图标/位置天然不变）。
- ww-launch 判定命中规则 → 打开提醒页；放行时以 `--real <原命令行>` 启动真实程序（防递归）。
- **次方案（可选保留）：.lnk 改写**，修复匹配（按 target 真实路径 + 基名回退）、写入失败回滚、失败写日志。
- **fail-safe**：无法确认状态时默认走提醒页并记录，不再静默放行。
- 需要管理员：配合 4.3 提权模型。

### 4.3 提权与自启
- manifest 改为 **requireAdministrator**（需要改网卡/写 HKLM）。避免每次操作弹 UAC。
- 开机自启改用 **计划任务（最高权限，onlogon）**，避免登录时 UAC 提示。
- 网卡启停直接用 netsh（进程已是管理员，无需 runas），并记录 `last_disabled` 到配置，供托盘“恢复上次禁用”。

### 4.4 网卡一键禁用 / 恢复
- 通用 `setAdapterEnabled(adapterId, enabled)`：WiFi 与 以太网 均支持；成功后写 config `network_state.last_disabled = {type,name,time}`。
- 托盘右键：禁用/启用 WiFi、禁用/启用 以太网、**恢复上次禁用的连接**。

### 4.5 API（在 v1 基础上增补）
- `GET /api/network/summary` → wifi{ssid,signal,connected} + ethernet[{name,connected,enabled}]
- `POST /api/network/adapter/disable|enable` {type:"wifi"|"wired", id}
- `POST /api/network/restore-last`
- `GET /api/intercept/status`（IFEO 生效的 exe 列表）
- 保留 config/wifi/available/bypass/shortcuts/stats 等。

### 4.6 构建与验证
- 工具链：MSVC 19.44（VS2022 Build Tools）+ CMake + Ninja。脚本 `scripts/build-msvc.bat`。
- **当前仓库 Cannot build under MSVC**（`ww-launch/main.cpp:380` 使用 GCC `__attribute__`；`wifi_detector.cpp:31` 缺 `<mutex>`）→ v2 必须修复以保证标准 C++ 可移植。
- 回归：`ww-smoke` 原生 smoke + `/api/*` 冒烟；前端 mock 预览。

## 5. 风险
- IFEO 需管理员且被杀软关注 → 提供开关与一键还原；保留 .lnk 次方案兜底。
- NLM 事件在部分虚拟网卡下不触发 → 保留 60s 兜底刷新（仍不碰 WLAN 定位 API）。
- 要求管理员可能被安全软件拦 → 提供“仅监控/不拦截”降级模式。
