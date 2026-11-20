@echo off
setlocal DisableDelayedExpansion

rem ============================================================
rem FlightGear 2024.1.1 启动脚本（Install 目录下唯一入口）
rem 编码：GBK（中文 Windows 本地代码页）；行尾：CRLF
rem       二者缺一都会导致 cmd 解析出错，切勿改存为 UTF-8 或 LF
rem 路径：全部由 %~dp0 推导，整体搬迁到任意盘符/任意目录均可直接运行
rem       （含空格、中文、甚至网络共享 UNC 路径）
rem 目录结构要求：
rem       <本脚本所在目录>\
rem           LaunchFlightGear.bat
rem           fgdata_2024_1\      （数据包，需含 version 文件）
rem           aircraft\           （机模）
rem           flightgear\bin\fgfs.exe
rem ============================================================

rem %~dp0 表示本批处理文件所在目录，末尾自带反斜杠；
rem 用 %~dp0 而不是写死路径，是迁移兼容的关键
set "FG_INSTALL=%~dp0"

rem 数据包根目录、机模目录、程序本体目录
set "FG_ROOT=%FG_INSTALL%fgdata_2024_1"
set "FG_AIRCRAFT=%FG_INSTALL%aircraft"
set "FG_BIN_DIR=%FG_INSTALL%flightgear\bin"

rem Qt 运行环境：QML 模块、平台插件、图像插件都随程序安装在 bin 目录下
rem 缺少这三项时，Qt Quick 启动器会白屏或直接退出
set "QML_IMPORT_PATH=%FG_BIN_DIR%\qml"
set "QML2_IMPORT_PATH=%FG_BIN_DIR%\qml"
set "QT_PLUGIN_PATH=%FG_BIN_DIR%"

rem 启动前自检：先查目录结构，给出明确中文提示，避免窗口一闪而过看不到原因
if not exist "%FG_BIN_DIR%\fgfs.exe" (
    echo [错误] 未找到主程序：%FG_BIN_DIR%\fgfs.exe
    echo 请确认本脚本与 flightgear、fgdata_2024_1、aircraft 位于同一目录下。
    pause
    exit /b 1
)
if not exist "%FG_ROOT%\version" (
    echo [错误] 未找到数据包：%FG_ROOT%
    echo 请确认 fgdata_2024_1 目录存在且包含 version 文件。
    pause
    exit /b 1
)

rem 用 pushd 而不是 cd /d 切换工作目录：
rem 脚本位于网络共享（UNC 路径）时 cd /d 会直接失败，pushd 会自动映射临时盘符
pushd "%FG_BIN_DIR%"

rem 启动模拟器：--launcher 打开启动器界面，--language=zh_CN 指定界面语言
"%FG_BIN_DIR%\fgfs.exe" --launcher --language=zh_CN

rem 记录退出码：非 0 时暂停，方便查看崩溃信息
set "FG_EXIT=%ERRORLEVEL%"

rem 恢复调用前的工作目录
popd

if not "%FG_EXIT%"=="0" (
    echo.
    echo [错误] fgfs.exe 异常退出，退出码：%FG_EXIT%
    pause
)

endlocal & exit /b %FG_EXIT%
