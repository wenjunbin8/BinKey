@echo off
rem ===================================================================
rem  BinKey - 刷新资源管理器图标缓存
rem  用途: 重新打包 BinKeySetup.exe 之后, 如果资源管理器里图标仍显示旧的,
rem        运行本脚本 (右键 -> 以管理员运行 不需要, 普通权限即可)。
rem  原理: 资源管理器把每个文件的图标缓存到
rem        %LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db,
rem        重启资源管理器不会删除这些缓存, 必须手动删除。
rem ===================================================================

echo 正在停止资源管理器...
taskkill /f /im explorer.exe >nul 2>nul
timeout /t 3 /nobreak >nul

echo 正在删除图标缓存...
del /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db" 2>nul

echo 正在重启资源管理器...
start "" explorer.exe

echo.
echo 完成。现在打开桌面 / 文件夹, BinKeySetup.exe 应显示 1.ico 的图标。
echo 如果仍然不对, 请把 BinKeySetup.exe 复制到一个新文件名再看一次。
pause
