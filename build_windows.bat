@echo off
:: 设置控制台代码页为UTF-8，解决中文乱码
chcp 65001 >nul
echo 正在编译CMM (CPU与内存模拟器) for Windows...

REM 设置编译选项
set OPTIMIZE=2
set DEBUG=0
set ARCH=x64

REM 处理命令行参数
:parse_args
if "%~1"=="" goto end_parse_args
if /i "%~1"=="-o0" set OPTIMIZE=0 & shift & goto parse_args
if /i "%~1"=="-o1" set OPTIMIZE=1 & shift & goto parse_args
if /i "%~1"=="-o2" set OPTIMIZE=2 & shift & goto parse_args
if /i "%~1"=="-o3" set OPTIMIZE=3 & shift & goto parse_args
if /i "%~1"=="-g" set DEBUG=1 & shift & goto parse_args
if /i "%~1"=="-x86" set ARCH=x86 & shift & goto parse_args
if /i "%~1"=="-x64" set ARCH=x64 & shift & goto parse_args
if /i "%~1"=="-h" goto show_help
if /i "%~1"=="--help" goto show_help
echo 未知参数: %~1
shift
goto parse_args

:show_help
echo 用法: %0 [选项]
echo 选项:
echo   -o0,-o1,-o2,-o3   设置优化级别 (默认: -o2)
echo   -g                启用调试信息
echo   -x86              编译32位可执行文件
echo   -x64              编译64位可执行文件 (默认)
echo   -h,--help         显示此帮助信息
exit /b 0

:end_parse_args

REM 检查是否安装了GCC
where gcc >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo 错误: 未找到GCC编译器。
    echo 请安装MinGW或MSYS2，并确保GCC在PATH环境变量中。
    exit /b 1
)

REM 设置编译参数
set CFLAGS=-Wall -O%OPTIMIZE%
if %DEBUG%==1 set CFLAGS=%CFLAGS% -g
if "%ARCH%"=="x86" set CFLAGS=%CFLAGS% -m32
if "%ARCH%"=="x64" set CFLAGS=%CFLAGS% -m64

echo 使用以下编译参数:
echo   优化级别: -O%OPTIMIZE%
echo   调试信息: %DEBUG%
echo   目标架构: %ARCH%
echo   完整参数: %CFLAGS%

REM 编译
echo 正在编译...
gcc %CFLAGS% -o cmm.exe main.c -lpsapi

if %ERRORLEVEL% neq 0 (
    echo 编译失败！
    exit /b 1
)

echo 编译成功！生成了cmm.exe
echo 用法示例: cmm.exe -c 50 -m 50

REM 如果有调试信息，显示文件大小
if %DEBUG%==1 (
    echo 警告: 已包含调试信息，文件较大。发布时请使用不带-g参数的版本。
)

echo 按任意键退出...
pause >nul 