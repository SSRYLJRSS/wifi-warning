# v2 拦截架构：IFEO 映像劫持（唯一机制）

目标：不再依赖 .lnk 替换；改为对"受管软件的 exe 文件名"写 IFEO 的 Debugger，实现**启动器无关**拦截
（默认桌面、开始菜单、飘雪桌面/SnowDesktop、命令行、更新器一律命中），且**对第三方桌面与受管软件自更新免疫**。

## 组成
- ww-launch.exe 作为 IFEO Debugger：系统启动受管 exe 时先运行它。argv[1]=目标 exe 全路径，其后为原始参数。
  流程：护栏(自身/系统关键进程)→读 config→未受管或保护关闭或网络不匹配→放行；命中规则→打开 /warning 页并**不启动**目标(=拦截)。
- 放行递归防护（ExecutionMaster 方案）：CreateProcess 用 DEBUG_PROCESS|DEBUG_ONLY_THIS_PROCESS 创建，
  立刻 NtRemoveProcessDebug / DebugActiveProcessStop 脱离，WaitForSingleObject 等待并回传退出码。
  遇 ERROR_ELEVATION_REQUIRED → ShellExecuteEx(runas) 重新进入本流程（提权实例可正常 CreateProcess）。
  仍失败→NTFS 同卷硬链接(不同文件名)兜底启动。
- core/ifeo_manager：注册表读写（64/32 位双写：默认视图 + Wow6432Node），状态文件 ifeo-state.json 记录原始 Debugger 以便精确还原；sync/verify/restore。
- ww-admin.exe（requireAdministrator）：--ifeo-sync / --ifeo-restore / --ifeo-remove <exe>，由主程序按需 runas 调用。
- 主程序 wifi-warning.exe 保持 asInvoker；申请/还原拦截时才弹 UAC。

## 安全与边界
- 护栏：绝不把 ww-launch/ww-admin/wifi-warning 及 csrss/wininit/winlogon/services/lsass/svchost/smss/explorer 设为受管。
- config 读不到时：仅放行"非受管"目标；受管目标不静默放行（fail-closed 到内置清单，再弹警告页）。
- 卸载/关闭保护：ww-admin --ifeo-restore 精确还原原始 Debugger 或删除键，避免"锁死"受管软件。
- 局限：IFEO 按映像文件名匹配（改名可绕）；UWP/商店应用不适用；非安全边界（原生 API 可绕）。
- 不签名：补齐 VERSIONINFO、正规文件名/安装目录、不加壳、行为光明正大、README 指引用户加白并提交误报申诉。
