# WiFiWarning v2.0 — 实施计划（Plan）

> 阶段：plan（planner） · 依据定稿文档：[docs/v2-research.md](docs/v2-research.md)（根因证据）、[docs/v2-architecture.md](docs/v2-architecture.md)（选型+架构）、[docs/v2-ux-spec.md](docs/v2-ux-spec.md)（前端规范）。三者直接采用，不推翻。
> 基线：本工作区 C++20 / Win32 托盘，CMake 项目 1.5.0；前端已初建 SPA 骨架。原则：稳定 / 低占用优先；拦截 fail-safe；后台不碰定位敏感 WLAN API；一屏可达 SPA。

## 0. 工作区快照（供 generate 参考）

- 已有未提交改动：`src/core/wifi_detector.cpp` 已加 `#include <mutex>`；`src/ww-launch/main.cpp` 已删除 GCC `__attribute__`；前端 SPA 初版（`frontend/index.html`、`css/app.css`、`js/app.js`、`js/i18n.js`，含 mock）。下列任务以“补齐 / 核对”落地，不推翻已有效改动。
- 现有 API（`src/ui/api_handlers.cpp:51-69`）：/api/config、/api/wifi/（current、available、switch）、/api/network/（current、wired、wired/toggle）、/api/stats、/api/bypass、/api/apps/*、/api/shortcuts/* —— **尚无** v2 新增接口。
- 托盘菜单（`src/ui/tray_icon.cpp:164-193`）：仅“当前网络 / 有线适配器启停 / 防护 / 设置 / 统计 / 退出”——需新增 WiFi 启停与“恢复上次禁用”。
- 现网卡逻辑（`src/core/network_manager.cpp`）：`listWiredAdapters` 仅取 `IF_TYPE_ETHERNET_CSMACD`，启停走 `ShellExecuteEx runas`（每次弹 UAC），无 `last_disabled` 记录。

## 1. 任务分解

### 1.1 后端组（src/）

| ID | 任务 | 涉及文件 | 改动要点 | 验收 |
|---|---|---|---|---|
| B1 | MSVC 可移植性 | `src/core/wifi_detector.cpp`、`src/ww-launch/main.cpp` | 核对 `#include <mutex>`（已在第 12 行，保留）；GCC `__attribute__((unused))` 改为标准 `[[maybe_unused]]`（工作区已删，确认无残留）；全仓无其它 GCC 扩展 | build-msvc 零错误 |
| B2 | 网络检测去定位 | `src/ui/tray_icon.cpp`、`src/core/wifi_detector.cpp` | 移除常驻 `WlanOpenHandle` + `WlanRegisterNotification`（tray_icon.cpp:45-57、382-389）；后台线程不再长期持有 WLAN 句柄；连通性改 NLM(INetworkListManager)+GetAdaptersAddresses；60s 兜底刷新不查 WLAN | 后台无定位常驻调用 |
| B3 | SSID 按需读取 | `src/core/wifi_detector.cpp`、`src/ui/api_handlers.cpp` | SSID 仅在打开面板/手动刷新时读一次；优先 `WlanConnectionProfileDetails.GetConnectedSsid`；新增“隐藏 SSID / 显示配置名”设置项，开启后彻底不调 current_connection | 设置开启后零 location 调用 |
| B4 | 通用网卡启停 | `src/core/network_manager.cpp/.h` | 新增通用 `setAdapterEnabled(type,id,enabled)`：wifi=`IF_TYPE_IEEE80211`、wired=`IF_TYPE_ETHERNET_CSMACD`（排除虚拟/蓝牙/VPN）；成功后写 config `network_state.last_disabled={type,name,time}`；主程序已提权直接 netsh，去掉每次 runas | wifi 与 wired 均可启停并记录 |
| B5 | 恢复上次禁用 | `src/core/network_manager.cpp`、`src/core/config_manager.cpp` | 读 `last_disabled` 重新启用对应适配器，成功后清除/更新记录；失败返回可读原因 | 恢复成功且记录更新 |
| B6 | 托盘新增项 | `src/ui/tray_icon.cpp` | 右键新增：禁用/启用 WiFi、禁用/启用 以太网、恢复上次禁用（对齐 UX §4）；复用 B4/B5，结果 Toast/消息框反馈 | 菜单项可见可用 |
| B7 | 新增 API | `src/ui/api_handlers.cpp/.h`、`src/ui/http_server.cpp` | 新增 `GET /api/network/summary`、`POST /api/network/adapter/disable\|enable`、`POST /api/network/restore-last`、`GET /api/intercept/status`；字段契约见 §2.3 | 接口返回契约字段 |
| B8 | 拦截 fail-safe | `src/ww-launch/main.cpp` | 移除静默 fail-open（无网络 / 无规则 / 浏览器或服务不可用默认放行）；不确定状态改为“打开提醒页并记录日志”，仅显式 bypass 或 protection_enabled=false 放行 | ww-smoke 覆盖不得静默放行 |
| B9 | 提权/自启 + 拦截状态 | `src/main.cpp`、`src/resources/app.manifest`、`src/core/auto_start.cpp`、`src/ui/api_handlers.cpp` | 主程序改 `requireAdministrator`；开机自启改计划任务（最高权限 onlogon）；`/api/intercept/status` 读取 IFEO Debugger 列表供状态展示。**IFEO 劫持本体列入后续阶段**，本阶段只保证状态接口 + fail-safe | manifest/自启/状态接口可用 |

### 1.2 前端组（frontend/）

| ID | 任务 | 涉及文件 | 改动要点 | 验收 |
|---|---|---|---|---|
| F1 | SPA 骨架核对 | `frontend/index.html`、`css/app.css`、`js/app.js` | 左侧 5 导航（概览/规则/组/动态/设置）+ 顶部状态条 + 底部防护开关，无页面跳转（UX §2/§5） | 5 视图可切换 |
| F2 | 概览页 | `js/app.js` | 主状态卡（WiFi SSID/信号 4 格、以太网名称/连通）+ 快捷按钮（禁用/启用 WiFi、以太网、恢复上次禁用）+ 防护大开关 + 今日/本周拦截（UX §3.1） | 按钮调用正确接口 |
| F3 | 规则/组/动态页 | `js/app.js` | 规则卡（网络→软件组、启停/编辑/删除）+ 新建三步抽屉；组卡应用 chips + 文件选择；动态过滤表 + 3 指标（UX §3.2-3.4） | CRUD 与过滤可用 |
| F4 | 设置页 | `js/app.js`、`index.html` | 通用/安全/高级/关于；高级含“拦截方式(IFEO/快捷方式)状态 + 一键恢复快捷方式 + 提权说明”（UX §3.5） | 各分区渲染完整 |
| F5 | API 层统一 | `js/api.js` | 抽统一 `API.get/post` 覆盖新接口（summary/adapter/restore-last/intercept/status），与后端契约一致；旧方法对齐或废弃 | 无重复/冲突调用 |
| F6 | mock 预览 | `js/app.js` | 无后端时 mock 全接口（已有雏形，补齐 summary/adapter/restore-last/intercept/status） | 无后端可完整预览 |
| F7 | i18n/主题 | `js/i18n.js`、`css/app.css` | 中英切换即时生效并持久化；深/浅主题；设计令牌对齐 UX §5 | 切换即时持久化 |

## 2. 验收标准（可验证）

1. **构建**：运行 `scripts/build-msvc.bat`，退出码 0，生成 `wifi-warning` / `ww-launch` / `ww-smoke` 三产物。
2. **Smoke**：`ww-smoke` 全通过（含 `WW_TEST_*` 假网络决策用例）。
3. **API 契约（字段级）**：
   - `GET /api/network/summary` → `{ok, wifi:{ssid,signal,connected,enabled,available}, ethernet:[{id,name,connected,enabled}], current:{type,id,name}}`
   - `POST /api/network/adapter/disable|enable` body `{type:"wifi"|"wired", id}` → `{ok, adapter:{type,id,name}, error?}`
   - `POST /api/network/restore-last` → `{ok, restored:{type,name}|null, error?}`
   - `GET /api/intercept/status` → `{ok, active, mode:"IFEO"|"shortcut", exes:[...]}`
   - 错误统一 `{ok:false, error:string}`
4. **拦截 fail-safe**：无网络 / 服务不可用 / 规则不命中 **不得静默放行**；默认走提醒页并写日志；仅显式 bypass 或 `protection_enabled=false` 放行（ww-smoke 覆盖）。
5. **前端 mock 预览**：无后端时用 `scripts/preview-server.js` 打开首页，5 视图可切换、按钮有反馈、无控制台报错；截图存 `docs/screenshots/`。
6. **定位**：后台稳定运行 ≥10 分钟不出现“定位使用中”常驻指示（人工/事件核实）。

## 3. 风险与回滚

- **IFEO 被杀软关注** → 提供显式开关与“一键还原所有劫持”，安装时记录原值；保留 .lnk 次方案兜底。
- **requireAdministrator 被杀软拦截** → 提供“仅监控/不拦截”降级模式（不写 IFEO、不改快捷方式，仅记录）。
- **NLM 事件在虚拟网卡不触发** → 保留 60s 兜底刷新（仅 NLM/GetAdaptersAddresses）。
- **回滚**：每任务独立提交；前后端解耦（前端 mock 可独立预览）；新接口失败时旧接口保持可用；提供“恢复快捷方式”与“清除 IFEO”入口。

## 4. 测试计划

1. **MSVC 构建**：运行 `scripts/build-msvc.bat`，留存构建日志（判定 §2.1）。
2. **原生 smoke**：运行 `ww-smoke`，覆盖网络决策与新增 API handler。
3. **API 冒烟**：起服务后以 PowerShell/curl 依次请求 §2.3 四组接口，断言字段与错误结构。
4. **前端截图**：`scripts/preview-server.js` + 浏览器自动化，截 5 视图（深/浅、中/英），存 `docs/screenshots/v2-*.png`。
5. **回归**：`tray-smoke`、`runtime-smoke`、`browser-smoke` 通过；定位指示人工核实。

## 5. 依赖与写域建议（供 generate 建任务）

- **顺序**：B1 先行（无依赖）→ B2..B9 与 F1..F7 可并行（前端 mock 不阻塞后端）。
- **写域**：后端 `src/core/network_manager.*`、`src/core/config_manager.*`、`src/core/wifi_detector.*`、`src/ui/{api_handlers,tray_icon,http_server}.*`、`src/ww-launch/main.cpp`、`src/main.cpp`、`src/resources/app.manifest`；前端 `frontend/**`（js/app.js、js/api.js、js/i18n.js、css/app.css、index.html）。两组写域不重叠，可并行。

## 6. 交付物

- `docs/v2-plan.md`（本文件）；generate 阶段产出源码改动 + `docs/screenshots/` 截图 + 更新 README/版本。
