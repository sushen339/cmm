# Makefile for cmm (CPU and Memory Monitor)

# 可自定义的编译选项
# 使用方法：make [debug=1] [optimize=3] [arch=x64]
optimize ?= 2
debug ?= 0
arch ?= native

# 根据操作系统选择编译器和标志
ifeq ($(OS),Windows_NT)
    CC = gcc
    CFLAGS = -Wall -O$(optimize)
    LDFLAGS = -lpsapi
    TARGET = cmm.exe
else
    CC = gcc
    CFLAGS = -Wall -O$(optimize)
    LDFLAGS = -lpthread -lm
    TARGET = cmm
endif

# 根据参数调整编译选项
ifeq ($(debug),1)
    CFLAGS += -g -DDEBUG
endif

# 处理架构选项
ifeq ($(arch),x86)
    CFLAGS += -m32
else ifeq ($(arch),x64)
    CFLAGS += -m64
else ifneq ($(arch),native)
    CFLAGS += -march=$(arch)
endif

# 源文件
SRCS = main.c

# 目标文件
OBJS = $(SRCS:.c=.o)

# 默认目标
all: info $(TARGET)

# 显示编译信息
info:
	@echo "正在编译CMM (CPU与内存模拟器)"
	@echo "编译选项:"
	@echo "  优化级别: -O$(optimize)"
	@echo "  调试信息: $(debug)"
	@echo "  目标架构: $(arch)"
	@echo "  编译标志: $(CFLAGS)"

# 链接
$(TARGET): $(OBJS)
	@echo "链接目标: $@"
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "编译完成！"
	@echo "使用方法: ./$(TARGET) -c 50 -m 50"

# 编译
%.o: %.c
	@echo "编译: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	@echo "清理项目..."
ifeq ($(OS),Windows_NT)
	-del /Q $(OBJS) $(TARGET) 2>NUL
else
	-rm -f $(OBJS) $(TARGET)
endif
	@echo "清理完成！"

# 调试版本快捷方式
debug:
	$(MAKE) debug=1

# 优化版本快捷方式
release:
	$(MAKE) optimize=3 debug=0

# 安装目标(仅限Linux/UNIX)
install: $(TARGET)
ifneq ($(OS),Windows_NT)
	@echo "安装到 /usr/local/bin/..."
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "安装完成！"
else
	@echo "Windows系统不支持安装命令"
endif

# 卸载目标(仅限Linux/UNIX)
uninstall:
ifneq ($(OS),Windows_NT)
	@echo "从 /usr/local/bin/ 中卸载..."
	rm -f /usr/local/bin/$(TARGET)
	@echo "卸载完成！"
else
	@echo "Windows系统不支持卸载命令"
endif

# 帮助信息
help:
	@echo "可用的make目标:"
	@echo "  all       - 编译项目(默认目标)"
	@echo "  clean     - 清理编译产物"
	@echo "  debug     - 编译调试版本"
	@echo "  release   - 编译高度优化的发布版本"
	@echo "  install   - 安装到系统(仅限Linux/UNIX)"
	@echo "  uninstall - 从系统卸载(仅限Linux/UNIX)"
	@echo "  help      - 显示此帮助信息"
	@echo ""
	@echo "可用的参数:"
	@echo "  debug=0/1     - 启用/禁用调试信息(默认: 0)"
	@echo "  optimize=0-3  - 设置优化级别(默认: 2)"
	@echo "  arch=x86/x64/native/... - 设置目标架构"
	@echo ""
	@echo "例子:"
	@echo "  make debug=1 optimize=0     - 编译未优化的调试版本"
	@echo "  make arch=x64 optimize=3    - 编译高度优化的64位版本"
	@echo "  make release                - 编译高度优化的发布版本"

# 防止目标名称与文件名冲突
.PHONY: all info clean debug release install uninstall help 