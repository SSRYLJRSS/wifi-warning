# WiFiWarning v2.0 — 研究：两大投诉的根因、拦截/提权/框架建议

> 阶段：research（只读源码，不改 src/ 或 frontend/）。基线：仓库 F:\vibecode\wifiwarning，CMake 项目版本 1.5.0（CMakeLists.txt:3），README 标称 v1.9.x，C++20 / Win32 托盘。
> 方法：逐文件阅读拦截链路与 WLAN 调用点 + 微软官方文档确认定位敏感性。所有“主张”均有 file:line 或官方 URL 证据，见第 8 节证据表。

## 0. 结论速览

- 投诉一“经常拦截不了”不是单点 bug，而是三层叠加：
  1) 拦截只改 .lnk（覆盖面结构性不足，且大量启动路径绕过）；
  2) ww-launch 全程“失败即放行（fail-open）”，任何一步不确定就启动真实程序、且常无提示；
  3) 精确路径/网络名匹配脆弱，app 更新、换路径、名字大小写或指向启动器都匹配不到。
  额外关键交叉根因：Win11 24H2 起 wlan_intf_opcode_current_connection 受定位权限管控，用户拒绝定位时 ww-launch 读不到 SSID → 判定“无网络” → 放行。也就是说“定位被拒”会直接导致“拦截失效”。
- 投诉二“定位服务一直提示”确证：WlanQueryInterface(wlan_intf_opcode_current_connection) 与 WlanGetAvailableNetworkList 被微软文档明确列为“无精确位置授权即 ERROR_ACCESS_DENIED、且调用会出现在托盘 in-use 与定位最近活动”的 API。现有代码在缓存过期(≤30s)、每次 ACM 通知、每 60s 兜底、每次打开 WiFi 选择页、每次 app 启动时都会调用它们，于是指示常驻。动态加载 wlanapi.dll 与 30s 缓存并不能规避（定位是按“调用”而非“链接方式”判定的）。
- 拦截主方案建议：IFEO 映像劫持（HKLM …\Image File Execution Options\<exe>\Debugger 指向 ww-launch），配合递归防护与一键还原；.lnk 改写降为次方案；WMI/ETW 进程创建监控仅做兜底/审计，不做主拦截。提权建议：主程序 requireAdministrator + 计划任务（登录时、最高权限）自启，ww-launch 保持 asInvoker。语言建议：继续 C++20/Win32 原生核心 + 静态 Web UI（占用与稳定性最优）。

## 1. 现有拦截链路（先讲清机制）

1. 用户建规则 → 前端调 POST /api/shortcuts/replace → shortcut_manager 把 .lnk 的 target 改写成 ww-launch.exe，并加 --app <真实exe> --rule <ruleId>（shortcut_manager.cpp:234-268，参数拼装 252-253）。
2. 用户点击被改写的 .lnk → 实际启动 ww-launch.exe。
3. ww-launch 读同目录 config.json（ww-launch/main.cpp:117-126, 783-786），判断 protection_enabled / bypass（788-814）。
4. ww-launch 自己枚举“当前活动网络”：WiFi 用 WlanQueryInterface(current_connection) 取 SSID（270-304），有线用 GetAdaptersAddresses（348-368）。
5. ww-launch 用自己内嵌的 JSON 解析器匹配规则：network_type + network_id 全等、original_path 与 --app 全等（438-527，比较点在 492-494、508）。
6. 命中 → 打开提醒页 http://localhost:<port>/warning（707-726, 884）；未命中/任何失败 → shellOpenWithArgs 直接启动真实程序（fail-open）。

注意：主程序（tray UI）里的 rule_engine.cpp 只服务于 /api/apps/status 等展示；真正的拦截判定是 ww-launch 内那份独立实现（两份逻辑必须保持一致，属于重复实现风险）。

## 2. 投诉一：拦截经常失效 — 根因与证据

### 2.1 结构性根因：只改 .lnk，覆盖不到多数启动方式
- 只扫描并改写少量目录的 .lnk：Desktop、Start Menu(Programs)、Common Start Menu、TaskBar 的 User Pinned（shortcut_manager.cpp:144-167）。**没有**包含 %APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\StartMenu（Win11“开始菜单磁贴”常用此目录），也没有任何对“开始菜单数据库/AppsFolder”的处理。
- 因此以下启动方式全部绕过拦截：直接双击 exe、开始菜单磁贴/搜索直接启动 exe、任务栏跳转列表/最近使用、Run 对话框、文件关联/协议处理、更新器或启动器（Steam/Epic/游戏启动器）、计划任务/服务、拖放、命令行。用户看到的“经常拦不住”主要来自这里。
- 替换是一次性的：app 更新或重装重建快捷方式后不会自动再替换；reconcileShortcutRecords（shortcut_manager.cpp:366-384）在“原 .lnk 或备份缺失”时静默丢弃替换记录，不会重新生效。

### 2.2 ww-launch 的 fail-open 放行路径（全清单）
按 ww-launch/main.cpp 执行顺序，任何一条命中都“直接启动真实程序”：
1. config 缺失/为空 或 protection_enabled=false → 放行（788-800，jsonBool 默认 true，但空文件仍走此分支）。
2. 存在有效 bypass（bypass_until_epoch 未到）→ 放行（802-814，hasActiveBypass 243-250）。提醒页“密码放行”就是写这个窗口，属设计内。
3. 活动网络数为 0 → 放行（832-845）。**这是最致命的一条**：currentSsid 在定位被拒/枚举失败/未连 WiFi 时返回空，WiFi 单网机器就变成 0 网络 → 放行。
4. 没匹配到规则 → 放行（848-861）。精确匹配失败（见 2.3）都落到这里。
5. 命中规则但提醒页打不开：url 非空且 httpAlive(port) 但 shellOpen 失败 → 先弹“无法打开浏览器警告页，应用将被允许启动”，然后放行（884-888，561）。
6. 服务不在（httpAlive=false）或 url 为空 → 弹“服务未运行，应用将被允许启动”，然后放行（889-892）。
- 补充：主程序启动时若 ww-launch.exe 缺失，会“还原所有快捷方式并把 protection 关掉”（main.cpp:143-148）；若判定“无 WiFi 适配器”也会关闭防护并还原全部快捷方式（main.cpp:157-166；tray_icon.cpp:136-157 disableProtectionForNoAdapter）。均导致之后彻底不拦截。

### 2.3 精确匹配脆弱（规则命中不了）
- 应用路径要求“规范化为绝对路径+小写后完全相等”（rule_engine.cpp:12-19, 34-44；ww-launch/main.cpp:443, 508）。以下都会不等：app 升级换了版本目录、路径中有非 ASCII 大小写差异（toLowerAscii 只处理 ASCII）、8.3 短路径、符号链接/大小写不同的盘符路径、.lnk 实际指向启动器而非 exe（target 为空则 findShortcutsForApp 根本不收录，见 readShortcut 返回 !target_path.empty()，71 行；183 行才收）。
- 网络名要求 network_type 与 network_id 完全相等且区分大小写（ww-launch/main.cpp:492-494 用 strcmp；rule_engine.cpp:38 用 != 比较）。有线规则的 network_id 来自“友好名”，而 ww-launch 用 GetAdaptersAddresses 的 FriendlyName、列表却经 netsh 别名（network_manager.cpp:135-171 vs 194-312），名称口径不一致时匹配不上。
- app 启动参数：若 .lnk 原本带参数，replaceShortcut 会把原参数放进 --app-args（252-253）并在放行时回放，但匹配仍只比对 exe 路径，指向同一 launcher 的多款游戏/多开场景无法区分。

### 2.4 缓存与自动关闭带来的“看似失效”
- wifi_detector 30s TTL（wifi_detector.cpp:15-31, 132-153）与 network_manager 30s 适配器 TTL（network_manager.cpp:23-48）：只影响托盘/接口展示最多 30s 陈旧。重要澄清：ww-launch 不使用该缓存（每次启动现查），所以“切网后立即启动被放行”并非 ww-launch 缓存导致；真正的“切网后不生效”来自 2.3 与 2.2-3。
- 但 TTL 会放大“无适配器即关闭防护”的竞态：若首次查询偶发拿到 0 个 WiFi 接口，tray 会关闭防护并还原全部快捷方式（tray_icon.cpp:136-157），之后长时间不拦截。

### 2.5 “拦截失效”的判定结论
主因排序：结构性只改 .lnk（2.1） > fail-open 设计（2.2） > 精确匹配脆弱（2.3） > 定位被拒导致读不到 SSID（见 3.3，跨投诉）> 缓存/自动关闭（2.4）。

## 3. 投诉二：定位服务一直提示 — 触发点与权威确认

### 3.1 代码中的全部定位敏感调用点
1. wifi_detector.cpp:100-107 —— WlanQueryInterface(wlan_intf_opcode_current_connection) 读 SSID（BSSID/SSID 属定位数据）。调用频率：缓存过期(≤30s，前端每 5s 轮询 /api/network/current，polling.js:1-16)、每次 WLAN ACM 通知后 refreshWifiCache(157-160 被 tray_icon.cpp:403 调用)、每 60s 兜底（tray_icon.cpp:391-409）。
2. wifi_switcher.cpp:68 —— WlanGetAvailableNetworkList（WiFi 扫描列表，强定位）。触发：打开 WiFi 选择页 → GET /api/wifi/available → availableNetworksJson → listAvailableWifiNetworks（api_handlers.cpp:135-143；wifi_switcher.cpp:283-285）。
3. ww-launch/main.cpp:270-304（294 行）—— 每次“被管控 app 启动”都调 current_connection，故每次启动也会闪一下定位。
4. tray_icon.cpp:45-57 —— WlanRegisterNotification(WLAN_NOTIFICATION_SOURCE_ACM)。注意：微软文档只把 WLAN_NOTIFICATION_SOURCE_MSM 与 wifiControl 能力（需定位授权）绑定，ACM 不在此列（见 3.2）。所以“长期持有句柄+注册 ACM”本身并非文档所述指示器来源；真正持续触发是上面的查询调用。
5. 其他非定位：WlanOpenHandle / WlanEnumInterfaces / WlanGetProfileList（profile 列表）未被列为定位敏感；GetAdaptersAddresses 不涉及 SSID，未被定位管控。

### 3.2 微软官方 CONFIRM（关键原文）
- 《Changes to API behavior for Wi-Fi access and location》(learn.microsoft.com/en-us/windows/win32/nativewifi/wi-fi-access-location-changes)：无精确位置授权时，下列 wlanapi.h 的 Win32 API 返回 ERROR_ACCESS_DENIED —— WlanGetAvailableNetworkList、WlanGetNetworkBssList、WlanQueryInterface（当 OpCode 为 wlan_intf_opcode_current_connection）、WlanScan；且“Calls to these APIs will appear in the in-use system tray and location recent activity.”。原文：WlanRegisterNotification is also affected, depending on the arguments you pass; WLAN_NOTIFICATION_SOURCE_MSM 需要 wiFiControl 能力，而该能力需要定位授权。
- 同页给出替代：若只需 SSID 可用 WlanConnectionProfileDetails.GetConnectedSsid；若需要位置请用 Geolocation API；“Don't make a Wi-Fi scan call unless strictly necessary.”
- 《WLAN_INTF_OPCODE》页：存在 wlan_intf_opcode_realtime_connection_quality，它“combines fields from wlan_intf_opcode_current_connection and WlanGetNetworkBssList, but it omits location-sensitive information, and for that reason it doesn't require location access privileges.”
- 结论：只要继续用 current_connection / WlanGetAvailableNetworkList，就必然与定位授权纠缠；动态 LoadLibrary(wlanapi.dll)（dyn_wlan.h:2 注释“avoids GPS/Location permission association”）是**无效规避**，定位是按 API 调用判定的。

### 3.3 交叉根因（同时解释两投诉）
- Win11 24H2 起，用户关闭定位 → current_connection 返回 ERROR_ACCESS_DENIED。ww-launch.currentSsid 忽略错误、SSID 为空 → getAllActiveNetworks 只剩有线；WiFi 单网机器 activeCount=0 → ww-launch 走 2.2-3 放行。于是“关定位 → 定位提示消失但拦截也失效”，正是两投诉并存的机理。
- 同理 tray 的 getCurrentWifi 会显示“未连接”，并可能触发 no-adapter 关闭防护。

### 3.4 推荐“不触发定位”的检测架构（替代持续 SSID 轮询）
- 连通/接口状态（后台常驻、零定位）：INetworkListManager（COM，NLM）事件 + GetAdaptersAddresses。NLM 提供连接性、网络类别；GetAdaptersAddresses 提供 IfType（IF_TYPE_IEEE80211 WiFi / IF_TYPE_ETHERNET_CSMACD 有线）、OperStatus、FriendlyName；两者都不返回 SSID/BSSID，不在微软定位管控清单。
- SSID（按需、可关闭）：仅在用户打开面板/手动刷新时读一次；优先 WlanConnectionProfileDetails.GetConnectedSsid；若仍用 Win32，则只在“用户主动操作”路径调用 current_connection（一次性），绝不在后台定时/事件轮询。提供“隐藏 SSID / 仅显示配置名”开关，彻底不碰定位 API。
- 事件驱动：用 NLM 的 INetworkListManager::Advise 或 INetworkListManagerEvents 感知网络变化，替代 WlanRegisterNotification；后台不长期持有 WLAN 句柄。
- 后台兜底刷新（60s）只查 NLM/GetAdaptersAddresses，不查 WLAN。
- 禁用/恢复 WiFi：netsh 或 WLAN 的“启用/禁用接口”属管理操作，不读 SSID，不触发定位。

## 4. C. 无驱动拦截方案对比与推荐

### 4.1 (i) .lnk 改写（现状）
- 优点：无管理员、无杀软告警、可逆、实现简单。
- 缺点：覆盖面结构性不足（见 2.1）；只能拦“被替换的那几个快捷方式”；匹配脆弱；app 更新重建即失效。适合作为“次方案/兜底”，不能作为主方案。

### 4.2 (ii) IFEO 映像劫持（建议主方案）
- 机制（官方/MITRE 佐证）：写 HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<image.exe>，值 Debugger(REG_SZ)=ww-launch.exe 路径。系统在创建该映像名进程时，改为先启动 ww-launch，并把目标映像路径作为第一个参数传入。与启动方式无关（双击、快捷方式、任务栏、更新器、Run 都生效）。不改快捷方式，名称/图标/位置天然不变。
- 优点：覆盖所有 CreateProcess 路径；不依赖快捷方式；无需驱动；管理员配置一次即可长期生效。
- 主要难点与解法：
  1) 递归启动：ww-launch 放行时若 CreateProcess 同一个 <image.exe>，会再次触发 IFEO → 死循环。解法 A（推荐）：为真实程序在临时目录建一个**不同文件名的 NTFS 硬链接**（同一文件、不同 basename，IFEO 按 basename 匹配故不触发），CreateProcess 该硬链接；解法 B（兜底）：在全局互斥锁保护下临时删除该 Debugger 值 → CreateProcess（映像加载在调用内同步完成）→ 立即恢复；窗口极小但并发启动期间可能被绕过。二者都要在 ww-launch 里识别“我是被 IFEO 调起的、argv[1] 是目标 exe”。
  2) 需管理员且可能被安全软件关注（MITRE 将其列为 T1546.012 IFEO Injection）：二进制签名、明确“解除全部劫持”入口、安装/卸载时记录并还原原值（原本无键则删除）。提供“仅监控/不拦截”降级。
  3) 覆盖面局限：IFEO 按**映像文件名**匹配 → 用户重命名 exe 可绕过；两个不同程序同名会被一起拦；UWP/Store 应用（经应用模型激活）通常不受 IFEO 约束；protected/系统进程不适用。这些由次方案与监控兜底。
  4) 目标须是 Win32 exe；指向启动器/带参数的快捷方式要解析出真实 exe。

### 4.3 (iii) 进程创建监控 + 终止（仅兜底/审计）
- WMI Win32_ProcessStartTrace（root\CIMV2，管理员）：事件含 ProcessID、ParentProcessID、ProcessName；要命令行/完整路径需再按 PID 查 Win32_Process（有竞态）。ETW Microsoft-Windows-Kernel-Process（GUID 22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716）的 ProcessStart 事件比 WMI 更早，但默认不含命令行（需 NT Kernel Logger）。
- 本质是“创建后再杀”，非“启动前拦截”：进程已执行首条指令，可能已做工作；短命进程易漏；非管理员监控看不到高权限进程；常驻订阅有 CPU/复杂度成本。适合作为“IFEO 漏网（改名/UWP）检测+记录/提示”，不建议作为主拦截。

### 4.4 推荐：主 IFEO + 次 .lnk 修复 + 兜底监控（仅告警）
主 IFEO 负责“启动前可靠拦截”；.lnk 修复版用于“用户拒绝管理员权限”或 UWP 场景；WMI/ETW 监控只做“发现绕过并记日志/提示”，不做静默终止。任何不确定状态默认“走提醒页并记录”，不再静默 fail-open（这是修复投诉一的中心原则）。

## 5. D. 提权模型

- 事实：netsh interface set interface admin=disable/enable 需要管理员；写 HKLM IFEO 也需要管理员。现状 manifest 为 asInvoker（src/resources/app.manifest:8），实际每次网卡开关用 ShellExecuteEx runas（network_manager.cpp:443-463）→ 每次都弹 UAC，体验割裂，且失败时无持久状态。
- 约束：ww-launch.exe 必须保持 asInvoker —— 它由用户双击启动，若它要管理员，则每次启动被管控软件都会弹 UAC，不可接受。
- 推荐：
  1) 两个 exe 不同 manifest：wifi-warning.exe（主程序）requireAdministrator；ww-launch.exe asInvoker。理由：主程序做 netsh / IFEO / 自启注册都需要管理员；ww-launch 只做判定+开浏览器，不需要特权。
  2) 开机自启用“计划任务（Run with highest privileges，触发器 At log on）”而非 Run 注册表/启动文件夹。理由：计划任务以最高权限启动不再逐次 UAC；一次创建需管理员，之后静默。避免登录时 UAC 弹窗。
  3) 主程序内直接调用 netsh（已提权，无需 runas），并持久化 last_disabled 供“恢复上次禁用”。
  4) 若用户未通过计划任务而手动启动主程序：用 ShellExecute runas 自提升一次。仅监控/不拦截的降级模式可不需要管理员。
- 备选（更强，成本更高）：把特权操作放进 LocalSystem 服务（netsh/IFEO 由服务执行），托盘 UI asInvoker 通过本地 IPC 调用。优点：多用户/无人登录也能强制；缺点：代码量与部署复杂度高。单体受控用途用“计划任务+requireAdministrator”足够。

## 6. E. 语言/框架取舍（验证）

| 方案 | 稳定 | 占用 | 仅 Win11 | 界面美观 | 关键事实/依赖 |
|---|---|---|---|---|---|
| C++20 / Win32 原生核心 + localhost 静态 Web UI（沿用+重建前端） | ★★★★★ | ★★★★★（常驻数 MiB，外部浏览器零内嵌内核） | ★★★★★ | ★★★★（Web UI 可做精美） | 无运行时依赖；托盘/网络原生；现有代码已如此 |
| C# / .NET + WinUI3/WPF | ★★★★ | ★★ | ★★★★ | ★★★★★（Mica/亚克力） | 自包含单文件体积大（数十~百 MB 级；MS 文档承认单文件很大，trim 可降）；WinUI3 需 Windows App SDK 运行时；内存较高 |
| Rust / Tauri | ★★★★ | ★★★ | ★★★★ | ★★★★ | 依赖系统 WebView2（Tauri 官方前置要求）；Win11 自带 WebView2 可缓解；Rust 托盘/异步复杂度、编译链成本 |
| Electron | ★★★ | ★（100MB+ 且 Chromium 常驻内存高） | ★★★ | ★★★★ | 体积/内存严重超标，直接排除 |

- 结论：在“稳定 + 占用少”硬约束下，C++20/Win32 核心 + 静态 Web UI 是唯一同时满足的路线；界面美观交给重建的前端。若未来想要“原生一体化窗口（Mica）”，可选嵌入式 WebView2（Win11 自带）作为可选壳，但要接受额外内存，不影响核心仍为 C++。
- 补充：当前前端是“多页 HTML + 跳转”，体验割裂（与 UX Spec 一致），v2 应改 SPA，但那是前端阶段工作。

## 7. 现有测试 / smoke 脚本与 mock 方式

- 原生单元/集成 smoke：tests/smoke.cpp（构建目标 ww-smoke，CMakeLists.txt:68-73）。它起本地 HttpServer、直接调 core 函数、并用 runLauncherWithEnv 以子进程运行 ww-launch 校验决策 JSON。
- 通过 WW_TEST_* 环境变量注入“假网络/假动作”，无需真实网卡：
  - WW_TEST_CURRENT_SSID / WW_TEST_CURRENT_SSID_FILE（wifi_detector.cpp:43-53；ww-launch/main.cpp:271）：伪造当前 SSID，支持 <disconnected>/<no-adapter>。
  - WW_TEST_CURRENT_NETWORK_TYPE / WW_TEST_CURRENT_NETWORK_ID（network_manager.cpp:59-86；ww-launch/main.cpp:325-336）：直接覆盖“当前网络”。
  - WW_TEST_WIRED_ADAPTERS_JSON（network_manager.cpp:88-108）：伪造有线网卡列表。
  - WW_TEST_WIRED_ACTION（network_manager.cpp:431-439）：伪造 netsh 启停结果（success/failure）。
  - WW_TEST_AVAILABLE_WIFI_JSON（wifi_switcher.cpp:39-54）：伪造 WiFi 扫描列表。
  - WW_TEST_CONNECT_WIFI（wifi_switcher.cpp:163-...）：伪造连接结果/password:模式。
  - WW_TEST_PICK_APPS_JSON / WW_TEST_PICK_SHORTCUTS_JSON（api_handlers.cpp:286,308）：伪造文件选择器。
  - WW_TEST_AUTOSTART_RUN_KEY（auto_start.cpp:13）：把自启写到临时注册表键，避免污染。
  - WW_TEST_SHORTCUT_REPLACE_FAIL（shortcut_manager.cpp:241）：强制快捷方式替换失败，测回滚。
  - WW_TEST_SHELLOPEN_FAIL / WW_TEST_NO_MESSAGEBOX / WW_TEST_FALLBACK_FILE（ww-launch/main.cpp:561,576,591）：测试浏览器/服务失败时的 fail-open 与落盘标记。
  - 非 WW_TEST 但用于隔离的：WW_SHORTCUT_ROOTS、WW_SHORTCUT_BACKUP_ROOT（shortcut_manager.cpp:146,201）。
- PowerShell 验收脚本（scripts/）：
  - runtime-smoke.ps1：--self-test-server / --self-test-tray 起服务/托盘后退出。
  - tray-smoke.ps1：托盘创建/菜单/自检；含 <no-adapter> 场景。
  - browser-smoke.ps1 + browser-smoke-runner.js：起服务并用浏览器跑设置页/警告页/选择页 CRUD。
  - local-acceptance.ps1 / external-acceptance.ps1 / elevated-wifi-acceptance.ps1 / real-shortcut-scan-smoke.ps1：本地/外部/提权 WiFi/真实快捷方式扫描验收。
  - validate-cmake.ps1、build-msvc.bat、build-mingw.ps1、package-portable.ps1：构建与打包。
- 构建现状（阻塞项，需 v2 修复）：
  - ww-launch/main.cpp:380 使用 GCC 扩展 __attribute__((unused))（MSVC 不支持）→ MSVC 编译失败。
  - wifi_detector.cpp:31 使用 std::mutex 但未 #include <mutex>（仅 atomic/chrono/memory）→ 严格编译器失败。
  - 这正是“改用 MSVC 需先修复”的证据。

## 8. 主张 → 来源 证据表

| # | 主张 | 证据（文件:行 / URL） |
|---|---|---|
| A1 | 拦截靠改写 .lnk 指向 ww-launch，参数 --app/--rule | src/core/shortcut_manager.cpp:234-268, 252-253 |
| A2 | 只扫描 Desktop/StartMenu/CommonStartMenu/TaskBar，未含 User Pinned\StartMenu | src/core/shortcut_manager.cpp:144-167 |
| A3 | 快捷方式须 target 精确相等才收；空 target 不收 | src/core/shortcut_manager.cpp:58-71, 183 |
| A4 | 更新/重建后静默丢弃替换记录 | src/core/shortcut_manager.cpp:366-384 |
| A5 | 规则按 network_type+network_id+original_path 精确(区分大小写)匹配 | src/core/rule_engine.cpp:12-46；src/ww-launch/main.cpp:492-494, 508 |
| A6 | ww-launch fail-open：config/disabled/bypass/无网络/无规则/浏览器失败/服务不在 均直接启动真实程序 | src/ww-launch/main.cpp:788-800, 802-814, 832-845, 848-861, 884-892 |
| A7 | 无 WiFi 适配器时关闭防护并还原全部快捷方式 | src/main.cpp:157-166；src/ui/tray_icon.cpp:136-157；src/core/wifi_detector.cpp:162-164 |
| A8 | ww-launch.exe 缺失时还原并关闭防护 | src/main.cpp:143-148 |
| A9 | 30s TTL 缓存（WiFi 与有线适配器） | src/core/wifi_detector.cpp:15-31,132-153；src/core/network_manager.cpp:23-48 |
| A10 | 前端每 5s 轮询网络状态 | frontend/js/polling.js:1-16,18-33 |
| B1 | WlanQueryInterface(current_connection) 属定位敏感，无授权 ERROR_ACCESS_DENIED，调用出现在托盘 in-use/定位记录 | MS Learn: https://learn.microsoft.com/en-us/windows/win32/nativewifi/wi-fi-access-location-changes |
| B2 | WlanGetAvailableNetworkList 属定位敏感（同上返回 ERROR_ACCESS_DENIED、出现在定位活动） | 同上 URL（明确列出 WlanGetAvailableNetworkList） |
| B3 | WlanRegisterNotification 受影响取决于参数；MSM 需 wiFiControl（需定位授权），ACM 未列 | 同上 URL |
| B4 | wlan_intf_opcode_realtime_connection_quality 不含定位敏感信息、不需定位权限 | MS Learn: https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/ne-wlanapi-wlan_intf_opcode |
| B5 | 官方建议：需要 SSID 用 WlanConnectionProfileDetails.GetConnectedSsid；需要位置用 Geolocation API；非必要不扫描 | B1 URL 的 "Action needed for the Wi-Fi APIs" 段 |
| B6 | 代码调用点：current_connection / 扫描 / 注册通知 | src/core/wifi_detector.cpp:100-107；src/core/wifi_switcher.cpp:68,283-285；src/ww-launch/main.cpp:294；src/ui/tray_icon.cpp:45-57 |
| B7 | 动态加载 DLL 声称规避定位——无效 | src/core/dyn_wlan.h:2（注释）；对照 B1（按调用判定，与链接方式无关） |
| C1 | IFEO Debugger 注册表键/值定义 | MS archive blog "Image File Execution Options (IFEO)"；MITRE ATT&CK T1546.012 https://attack.mitre.org/techniques/T1546/012/ |
| C2 | WMI Win32_ProcessStartTrace 属性 ProcessID/ParentProcessID/ProcessName | MS Learn: https://learn.microsoft.com/en-us/previous-versions/windows/desktop/krnlprov/win32-processstarttrace |
| C3 | ETW Microsoft-Windows-Kernel-Process 提供 ProcessStart 事件，比 WMI 早，但默认不含命令行 | 公开 ETW 资料（velociraptor-docs windows.etw.kernelprocess；StackOverflow 73545522） |
| D1 | 主程序当前 asInvoker，网卡开关每次 ShellExecute runas（弹 UAC） | src/resources/app.manifest:8；src/core/network_manager.cpp:443-463 |
| E1 | Tauri 依赖系统 WebView2（Windows 前置要求） | https://v2.tauri.app/start/prerequisites/ |
| E2 | .NET 自包含单文件体积大（MS 文档承认；trim 可减小） | https://learn.microsoft.com/en-us/dotnet/core/deploying/single-file/overview |
| T1 | 构建阻塞：GCC __attribute__ 与缺 <mutex> | src/ww-launch/main.cpp:380；src/core/wifi_detector.cpp:31 |
| T2 | 测试用 WW_TEST_* 注入假网络 | tests/smoke.cpp:637-665, 819-834, 755-757；各 core 文件中 getEnvWide(WW_TEST_*) |

## 9. 风险 / 待确认

- IFEO 可能被安全软件拦截或告警（与“注入”技术同源，MITRE T1546.012）；须签名、显式开关、一键还原、安装时保存原值。
- UWP/Store 应用与“重命名 exe”不受 IFEO 约束，需监控兜底或明确不支持。
- 定位“完全零调用”与“显示 SSID”不可兼得：SSID 本身是定位敏感数据；方案是“后台不查、按需查、可隐藏”，而不是假装动态加载能规避。
- 本报告为只读研究；v2-architecture.md 中“30s 缓存导致 ww-launch 切网后放行”的说法应更正为“缓存只影响展示与无适配器自动关闭竞态，ww-launch 不走缓存”（见 2.4）。
