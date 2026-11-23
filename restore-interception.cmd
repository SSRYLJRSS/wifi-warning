@echo off
setlocal
cd /d "%~dp0"
echo 正在以管理员权限还原 WiFi 提醒的 IFEO 拦截...
"%~dp0ww-admin.exe" --ifeo-restore
echo.
echo 完成。按任意键退出。
pause >nul
