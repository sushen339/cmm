# CPU与内存模拟器 (CMM)

这是一个由 AI 编写的跨平台工具，用于动态模拟系统的CPU和内存占用率。它会根据当前系统的实际负载，动态调整自身的资源消耗，以维持系统总体的CPU和内存使用率在指定目标值。

## 特性

- 支持Windows和Linux平台
- 动态调整CPU占用率
- 动态调整内存占用率
- 自动检测当前系统资源使用情况
- 实时显示系统状态

## 编译

### 在Linux上编译

```bash
make
```

### 在Windows上编译

使用MinGW或类似工具：

```bash
mingw32-make
```

或使用Visual Studio开发者命令提示符：

```bash
nmake /f Makefile
```

## 使用方法

```
./cmm -c <cpu_usage> -m <memory_usage>
```

参数说明：
- `-c <cpu_usage>`: 目标CPU使用率（百分比，0-100）
- `-m <memory_usage>`: 目标内存使用率（百分比，0-100）
- `-d`: 后台运行
- `-k`: 查找并终止所有正在运行的CMM进程
- `-h`: 显示帮助信息

例如，要使系统整体CPU和内存维持在50%的使用率：

```bash
./cmm -c 50 -m 50 -d
```

要终止所有运行中的CMM进程：

```bash
./cmm -k
```

## 工作原理

程序会实时检测当前系统的CPU和内存使用情况：

- 如果系统当前的资源使用率低于目标值，程序会消耗额外的资源以达到目标
- 如果系统当前的资源使用率已经达到或超过目标值，程序会减少自身的资源消耗

这样，无论系统上运行什么其他程序，CMM都会尝试保持总体系统资源使用率接近目标值。

## 退出程序

按下 `Ctrl+C` 可以安全退出程序。程序会释放所有分配的资源。 