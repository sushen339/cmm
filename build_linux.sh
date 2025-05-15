#!/bin/bash

echo "正在编译CMM (CPU与内存模拟器) for Linux..."

# 设置默认编译选项
OPTIMIZE=2
DEBUG=0
ARCH="native"

# 处理命令行参数
show_help() {
    echo "用法: $0 [选项]"
    echo "选项:"
    echo "  -o0,-o1,-o2,-o3   设置优化级别 (默认: -o2)"
    echo "  -g                启用调试信息"
    echo "  -m32              编译32位可执行文件"
    echo "  -m64              编译64位可执行文件"
    echo "  -march=ARCH       设置目标CPU架构"
    echo "  -h,--help         显示此帮助信息"
    exit 0
}

while [ "$1" != "" ]; do
    case $1 in
        -o0) OPTIMIZE=0 ;;
        -o1) OPTIMIZE=1 ;;
        -o2) OPTIMIZE=2 ;;
        -o3) OPTIMIZE=3 ;;
        -g)  DEBUG=1 ;;
        -m32) ARCH="32" ;;
        -m64) ARCH="64" ;;
        -march=*) ARCH="${1#-march=}" ;;
        -h|--help) show_help ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
    shift
done

# 检查是否安装了GCC
if ! command -v gcc &> /dev/null; then
    echo "错误: 未找到GCC编译器。"
    echo "请安装GCC: sudo apt-get install gcc (Debian/Ubuntu) 或 sudo yum install gcc (CentOS/RHEL)"
    exit 1
fi

# 设置编译参数
CFLAGS="-Wall -O$OPTIMIZE"

if [ $DEBUG -eq 1 ]; then
    CFLAGS="$CFLAGS -g"
fi

if [ "$ARCH" = "32" ]; then
    CFLAGS="$CFLAGS -m32"
elif [ "$ARCH" = "64" ]; then
    CFLAGS="$CFLAGS -m64"
elif [ "$ARCH" != "native" ]; then
    CFLAGS="$CFLAGS -march=$ARCH"
fi

echo "使用以下编译参数:"
echo "  优化级别: -O$OPTIMIZE"
echo "  调试信息: $DEBUG"
echo "  目标架构: $ARCH"
echo "  完整参数: $CFLAGS"

# 编译
echo "正在编译..."
gcc $CFLAGS -o cmm main.c -lpthread -lm

if [ $? -ne 0 ]; then
    echo "编译失败！"
    exit 1
fi

# 设置可执行权限
chmod +x cmm

echo "编译成功！生成了cmm"

# 如果有调试信息，显示警告
if [ $DEBUG -eq 1 ]; then
    echo "警告: 已包含调试信息，文件较大。发布时请使用不带-g参数的版本。"
    # 显示文件大小
    ls -lh cmm | awk '{print "可执行文件大小: " $5}'
else
    # 显示文件大小
    ls -lh cmm | awk '{print "可执行文件大小: " $5}'
fi

echo "用法示例: ./cmm -c 50 -m 50" 