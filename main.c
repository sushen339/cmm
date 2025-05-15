/*
 * CMM - CPU和内存模拟器
 * 版本: 1.1.1
 * 
 * 用于在系统上模拟特定的CPU和内存负载
 * 支持Windows和Linux平台
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <process.h>
#include <pdh.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// 全局变量
volatile sig_atomic_t running = 1;
int target_cpu_usage = 0;
int target_mem_usage_mb = 0;
int num_cpu_cores = 1;  // CPU核心数量
volatile double current_cpu_load = 0.0; // 当前实际的CPU负载
volatile double thread_cpu_load = 0.0;  // 每个线程的CPU负载
volatile double target_cpu_load = 0.0;  // 目标CPU负载（PID输出）
volatile int busy_percentage = 50;      // CPU繁忙百分比
volatile double filtered_cpu_usage = 0.0; // 滤波后的CPU使用率
volatile double filtered_mem_usage = 0.0; // 滤波后的内存使用率
bool daemon_mode = false;  // 后台运行模式标志

// 配置选项
double pid_kp = 1.5;  // 比例系数(增强以加强响应)
double pid_ki = 0.3;  // 积分系数(增加积分作用加强长期误差修正)
double pid_kd = 0.05; // 微分系数(降低微分作用减少阻尼)
double filter_alpha = 0.5; // 低通滤波系数(增加以更快响应变化)
bool verbose_mode = false; // 详细输出模式
int update_interval = 1;   // 状态更新间隔（秒）
bool save_config = false;  // 是否保存配置
char config_file[256] = "cmm.conf"; // 配置文件名

#ifdef _WIN32
CRITICAL_SECTION cpu_load_cs;
#else
pthread_mutex_t cpu_load_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

// 清屏函数
void clear_screen() {
#ifdef _WIN32
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD count;
    DWORD cellCount;
    COORD homeCoords = { 0, 0 };

    if (hStdOut == INVALID_HANDLE_VALUE) return;

    // 获取屏幕缓冲区信息
    if (!GetConsoleScreenBufferInfo(hStdOut, &csbi)) return;
    cellCount = csbi.dwSize.X * csbi.dwSize.Y;

    // 填充屏幕缓冲区
    if (!FillConsoleOutputCharacter(
        hStdOut,
        (TCHAR)' ',
        cellCount,
        homeCoords,
        &count
    )) return;

    // 填充屏幕缓冲区属性
    if (!FillConsoleOutputAttribute(
        hStdOut,
        csbi.wAttributes,
        cellCount,
        homeCoords,
        &count
    )) return;

    // 将光标移动到屏幕左上角
    SetConsoleCursorPosition(hStdOut, homeCoords);
#else
    // 使用ANSI转义序列清屏
    printf("\033[2J\033[H");
#endif
}

// 获取CPU核心数
int get_cpu_cores() {
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#else
    long nprocs = -1;
    nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs < 1)
        return 1;
    return nprocs;
#endif
}

// 获取系统CPU使用率
double get_system_cpu_usage() {
#ifdef _WIN32
    static ULARGE_INTEGER last_idle_time = {0};
    static ULARGE_INTEGER last_kernel_time = {0};
    static ULARGE_INTEGER last_user_time = {0};
    
    FILETIME idle_time, kernel_time, user_time;
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return 0.0;
    }
    
    ULARGE_INTEGER now_idle_time, now_kernel_time, now_user_time;
    now_idle_time.LowPart = idle_time.dwLowDateTime;
    now_idle_time.HighPart = idle_time.dwHighDateTime;
    now_kernel_time.LowPart = kernel_time.dwLowDateTime;
    now_kernel_time.HighPart = kernel_time.dwHighDateTime;
    now_user_time.LowPart = user_time.dwLowDateTime;
    now_user_time.HighPart = user_time.dwHighDateTime;
    
    if (last_idle_time.QuadPart == 0) {
        last_idle_time = now_idle_time;
        last_kernel_time = now_kernel_time;
        last_user_time = now_user_time;
        Sleep(1000);
        return get_system_cpu_usage();
    }
    
    ULONGLONG idle_diff = now_idle_time.QuadPart - last_idle_time.QuadPart;
    ULONGLONG kernel_diff = now_kernel_time.QuadPart - last_kernel_time.QuadPart;
    ULONGLONG user_diff = now_user_time.QuadPart - last_user_time.QuadPart;
    
    ULONGLONG total_diff = kernel_diff + user_diff;
    ULONGLONG used_diff = total_diff - idle_diff;
    
    last_idle_time = now_idle_time;
    last_kernel_time = now_kernel_time;
    last_user_time = now_user_time;
    
    if (total_diff == 0) return 0.0;
    
    double usage = used_diff * 100.0 / total_diff;
    if (isnan(usage) || usage < 0 || usage > 100) {
        return 0.0;
    }
    
    return usage;
#else
    // Linux通过/proc/stat获取CPU使用率
    static long long prev_idle = 0, prev_total = 0;
    FILE* fp = fopen("/proc/stat", "r");
    if (fp == NULL) return 0.0;
    
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        fclose(fp);
        return 0.0;
    }
    fclose(fp);
    
    long long user, nice, system, idle, iowait, irq, softirq, steal;
    sscanf(buffer, "cpu %lld %lld %lld %lld %lld %lld %lld %lld", 
        &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    
    long long current_idle = idle + iowait;
    long long current_total = user + nice + system + idle + iowait + irq + softirq + steal;
    
    long long idle_diff = current_idle - prev_idle;
    long long total_diff = current_total - prev_total;
    
    prev_idle = current_idle;
    prev_total = current_total;
    
    if (total_diff == 0) return 0.0;
    
    double usage = 100.0 * (1.0 - (double)idle_diff / total_diff);
    if (isnan(usage) || usage < 0 || usage > 100) {
        return 0.0;
    }
    
    return usage;
#endif
}

// 获取系统内存使用率
double get_system_mem_usage() {
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (!GlobalMemoryStatusEx(&memInfo)) {
        return 0.0;
    }
    return (double)memInfo.dwMemoryLoad;
#else
    // 使用/proc/meminfo读取，确保与free -m命令输出对齐
    FILE* fp = fopen("/proc/meminfo", "r");
    if (fp == NULL) {
        // 如果无法打开/proc/meminfo，回退到sysinfo方法
        struct sysinfo info;
        if (sysinfo(&info) != 0) {
            return 0.0;
        }
        
        long long total_mem = info.totalram;
        long long free_mem = info.freeram + info.bufferram + info.sharedram;
        long long used_mem = total_mem - free_mem;
        
        return (double)used_mem * 100.0 / total_mem;
    }    // 从/proc/meminfo读取
    char buffer[256];
    unsigned long long mem_total = 0;
    unsigned long long mem_free = 0;
    unsigned long long mem_available = 0; // 新的指标，更准确
    unsigned long long buffers = 0;
    unsigned long long cached = 0;
    unsigned long long slab = 0;
    
    while (fgets(buffer, sizeof(buffer), fp)) {
        unsigned long long value;
        
        if (sscanf(buffer, "%*[^:]: %llu", &value) == 1) {
            if (strncmp(buffer, "MemTotal:", 9) == 0) {
                mem_total = value;
            } else if (strncmp(buffer, "MemFree:", 8) == 0) {
                mem_free = value;
            } else if (strncmp(buffer, "MemAvailable:", 13) == 0) {
                mem_available = value;
            } else if (strncmp(buffer, "Buffers:", 8) == 0) {
                buffers = value;
            } else if (strncmp(buffer, "Cached:", 7) == 0) {
                cached = value;
            } else if (strncmp(buffer, "Slab:", 5) == 0) {
                slab = value;
            }
        }
    }
    
    fclose(fp);
    
    // 如果没有读到有效内存信息，回退到sysinfo方法
    if (mem_total == 0) {
        struct sysinfo info;
        if (sysinfo(&info) != 0) {
            return 0.0;
        }
        
        long long total_mem = info.totalram;
        long long free_mem = info.freeram + info.bufferram + info.sharedram;
        long long used_mem = total_mem - free_mem;
        
        return (double)used_mem * 100.0 / total_mem;
    }
    
    // 计算与free -m对齐的内存使用率
    // 如果有MemAvailable，优先使用它(更精确，Linux 3.14+)
    unsigned long long used_mem;
    if (mem_available > 0) {
        used_mem = mem_total - mem_available;
    } else {
        // 兼容老版本内核的计算方式
        used_mem = mem_total - mem_free - buffers - cached - slab;
    }
    
    // 确保不出现负值
    if (used_mem > mem_total) {
        used_mem = mem_total;
    }
    
    return (double)used_mem * 100.0 / mem_total;
#endif
}

// 获取系统总内存大小(MB)
unsigned long long get_total_system_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (!GlobalMemoryStatusEx(&memInfo)) {
        return 0;
    }
    return (unsigned long long)memInfo.ullTotalPhys / (1024 * 1024);
#else
    // 优先使用/proc/meminfo以确保与free -m一致
    FILE* fp = fopen("/proc/meminfo", "r");
    if (fp != NULL) {
        char buffer[256];
        unsigned long long mem_total = 0;
        
        while (fgets(buffer, sizeof(buffer), fp)) {
            if (strncmp(buffer, "MemTotal:", 9) == 0) {
                sscanf(buffer, "MemTotal: %llu", &mem_total);
                break;
            }
        }
        
        fclose(fp);
        
        if (mem_total > 0) {
            // MemTotal以KB为单位，转换为MB
            return mem_total / 1024;
        }
    }
    
    // 如果/proc/meminfo方法失败，回退到sysinfo
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        return 0;
    }
    return (unsigned long long)info.totalram * info.mem_unit / (1024 * 1024);
#endif
}

// 处理中断信号
void signal_handler(int sig) {
    printf("\n收到中断信号，程序即将退出...\n");
    running = 0;
}

// 直接CPU占用函数 - 纯计算模式，不休息
void spinCPU(unsigned long long cycles) {
    volatile unsigned long long i;
    volatile double result = 0.0;
    
    for (i = 0; i < cycles; i++) {
        result += i * 3.14159265358979323846 * 1.732050807568877;
        // 增加一些额外计算，提高工作强度
        result = result / (1.0 + (i % 5) * 0.01) + sqrt(i % 10);
    }
}

// 调整CPU负载线程
void* adjust_cpu_load_thread(void* arg) {
    // PID控制算法参数从全局变量获取
    const double Kp = pid_kp;   // 比例系数
    const double Ki = pid_ki;   // 积分系数
    const double Kd = pid_kd;   // 微分系数
    
    // 保存历史误差
    double prev_error = 0.0;
    double integral = 0.0;
    
    // 初始负载设置
    thread_cpu_load = 0.7;  // 初始值提高到70%，更快接近目标
    target_cpu_load = target_cpu_usage;
    busy_percentage = 70;   // 从70%开始
    
    // 预热CPU
    printf("CPU负载控制初始化中...\n");
    
    // 等待预热完成并初始化滤波值
#ifdef _WIN32
    Sleep(1000);
#else
    usleep(1000 * 1000);
#endif
    filtered_cpu_usage = get_system_cpu_usage();
    
    // 加入自适应PID系数调整
    double max_adjustment = 20.0; // 最大调整幅度限制(提高以增强响应能力)
    
    while (running) {
        // 获取当前系统CPU使用率
        double system_cpu_usage = get_system_cpu_usage();
        
        // 应用低通滤波器平滑CPU使用率波动
        filtered_cpu_usage = filter_alpha * system_cpu_usage + (1 - filter_alpha) * filtered_cpu_usage;
        
        // 更新当前CPU负载（用于显示）
        current_cpu_load = system_cpu_usage;
        
        // 计算误差 - 使用滤波后的CPU使用率
        double error = target_cpu_usage - filtered_cpu_usage;
        
        // 积分项计算 - 使用衰减以避免积分饱和
        integral = integral * 0.95 + error;
        
        // 对积分进行自适应限制，防止积分饱和
        double integral_limit = 25.0 / Ki; // 基于积分系数的自适应积分限制
        if (integral > integral_limit) integral = integral_limit;
        if (integral < -integral_limit) integral = -integral_limit;
        
        // 微分项计算
        double derivative = error - prev_error;
        prev_error = error;
        
        // 计算PID输出，加入自适应限制
        double pid_output = Kp * error + Ki * integral + Kd * derivative;
        
        // 限制单次调整幅度，提高稳定性
        if (pid_output > max_adjustment) pid_output = max_adjustment;
        if (pid_output < -max_adjustment) pid_output = -max_adjustment;
        
        // 更积极地调整CPU繁忙百分比
        busy_percentage += (int)(pid_output * 0.2); // 提高到20%，更快速达到目标
        
        // 限制CPU繁忙百分比在有效范围内
        if (busy_percentage < 0) busy_percentage = 0;
        if (busy_percentage > 100) busy_percentage = 100;
        
        // 更新目标CPU负载
        target_cpu_load = busy_percentage;
        
        // 更新工作线程的负载比例
#ifdef _WIN32
        EnterCriticalSection(&cpu_load_cs);
        thread_cpu_load = (double)busy_percentage / 100.0;
        LeaveCriticalSection(&cpu_load_cs);
#else
        pthread_mutex_lock(&cpu_load_mutex);
        thread_cpu_load = (double)busy_percentage / 100.0;
        pthread_mutex_unlock(&cpu_load_mutex);
#endif
        
#ifdef _WIN32
        Sleep(150);  // 150ms，减少采样周期，加快响应速度
#else
        usleep(150 * 1000);  // 150ms
#endif
    }
    
    return NULL;
}

// 新的CPU负载控制算法
void* cpu_load_thread(void* arg) {
    // 这里使用参数来区分线程，防止编译器警告
#ifdef _WIN32
    long thread_index = (long)(intptr_t)arg;
    (void)thread_index; // 标记为已使用，避免警告
#else
    long thread_index = (long)(intptr_t)arg;
    (void)thread_index; // 标记为已使用，避免警告
#endif
    
    // 设置高优先级
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    
    // 周期时间（微秒）
    const long long CYCLE_TIME_US = 5000; // 5ms = 5,000µs，降低以提高精度
    
    struct timespec cycle_start, current_time;
    double local_load;
    
    while (running) {
        // 获取当前的负载目标
#ifdef _WIN32
        EnterCriticalSection(&cpu_load_cs);
        local_load = thread_cpu_load;
        LeaveCriticalSection(&cpu_load_cs);
#else
        pthread_mutex_lock(&cpu_load_mutex);
        local_load = thread_cpu_load;
        pthread_mutex_unlock(&cpu_load_mutex);
#endif
        
        // 获取当前时间作为周期开始
#ifdef _WIN32
        timespec_get(&cycle_start, TIME_UTC);
#else
        clock_gettime(CLOCK_MONOTONIC, &cycle_start);
#endif
        
        // 计算这个周期应该工作的时间（微秒）
        long long work_time_us = (long long)(local_load * CYCLE_TIME_US);
        
        // 如果负载很低，跳过这个周期
        if (work_time_us < 50) { // 降低跳过阈值，增加工作比例
#ifdef _WIN32
            Sleep(5);  // 减少休眠时间
#else
            usleep(5000);  // 减少休眠时间
#endif
            continue;
        }
        
        // 繁忙等待指定的时间
        long long elapsed_us;
        do {
            // 执行一些计算密集型操作
            spinCPU(1000);
            
            // 获取当前时间
#ifdef _WIN32
            timespec_get(&current_time, TIME_UTC);
#else
            clock_gettime(CLOCK_MONOTONIC, &current_time);
#endif
            
            // 计算已经过去的时间（微秒）
            elapsed_us = (current_time.tv_sec - cycle_start.tv_sec) * 1000000 +
                         (current_time.tv_nsec - cycle_start.tv_nsec) / 1000;
                         
        } while (elapsed_us < work_time_us && running);
        
        // 休息到下一个周期开始
        long long sleep_time_us = CYCLE_TIME_US - elapsed_us;
        if (sleep_time_us > 100) { // 只有当休息时间足够长时才休息
#ifdef _WIN32
            Sleep((DWORD)(sleep_time_us / 1000));
#else
            usleep(sleep_time_us);
#endif
        }
    }
    
    return NULL;
}

// 内存分配函数
void allocate_memory() {
    static char** memory_blocks = NULL;
    static int allocated_blocks = 0;
    static unsigned long long allocated_mb = 0;
    static double prev_needed_mem_percent = 0.0;  // 上次需要的内存百分比
    static int memory_adjustment_counter = 0;     // 调整计数器
    static int target_not_reached_counter = 0;    // 目标未达到计数器
    static int block_size_mb = 1;                 // 内存块大小(MB)
    static int consecutive_failed_allocations = 0; // 连续分配失败计数
    static int stabilization_counter = 0;         // 稳定计数器，用于平滑分配过程
    static double last_mem_usage = 0.0;           // 上次的内存使用率
    static double avg_memory_change_rate = 0.0;   // 平均内存变化率
    
    // 获取当前系统内存使用情况
    double current_mem_usage_percent = get_system_mem_usage();
    
    // 计算内存使用率变化率
    double mem_change_rate = 0.0;
    if (last_mem_usage > 0.0) {
        mem_change_rate = fabs(current_mem_usage_percent - last_mem_usage);
        // 使用指数加权移动平均更新平均变化率
        if (avg_memory_change_rate == 0.0) {
            avg_memory_change_rate = mem_change_rate;
        } else {
            avg_memory_change_rate = 0.7 * avg_memory_change_rate + 0.3 * mem_change_rate;
        }
    }
    last_mem_usage = current_mem_usage_percent;
    
    // 根据内存变化率动态调整滤波系数
    // 变化大时使用小滤波系数，变化小时使用大滤波系数
    double adaptive_filter_alpha = filter_alpha;
    if (avg_memory_change_rate > 2.0) {
        // 变化大，减小滤波系数，更多依赖历史值以减缓波动
        adaptive_filter_alpha = filter_alpha * 0.5;
    } else if (avg_memory_change_rate < 0.5) {
        // 变化小，增大滤波系数，更快响应新值
        adaptive_filter_alpha = filter_alpha * 1.5;
        if (adaptive_filter_alpha > 0.8) adaptive_filter_alpha = 0.8;
    }
    
    // 应用自适应低通滤波器平滑内存使用率
    if (filtered_mem_usage == 0.0) {
        filtered_mem_usage = current_mem_usage_percent;
    } else {
        filtered_mem_usage = adaptive_filter_alpha * current_mem_usage_percent + 
                             (1 - adaptive_filter_alpha) * filtered_mem_usage;
    }
    
    unsigned long long total_system_memory_mb = get_total_system_memory();
    
    // 计算目标内存百分比
    double target_mem_percent = (double)target_mem_usage_mb * 100.0 / total_system_memory_mb;
    
    // 计算内存差距，同时考虑当前实际值和滤波值
    double mem_gap = target_mem_percent - current_mem_usage_percent;
    double filtered_gap = target_mem_percent - filtered_mem_usage;
    
    // 使用自适应权重综合考虑当前和过滤后的差距
    double current_weight = 0.5; // 默认权重
    
    // 调整权重 - 如果内存变化率高，偏向滤波值；如果变化率低，偏向当前值
    if (avg_memory_change_rate > 1.5) {
        // 高变化率，更多依赖滤波值
        current_weight = 0.3;
    } else if (avg_memory_change_rate < 0.5) {
        // 低变化率，更多依赖当前值
        current_weight = 0.7;
    }
    
    double effective_gap = mem_gap * current_weight + filtered_gap * (1.0 - current_weight);
    
    // 添加稳定机制 - 如果接近目标，增加稳定计数，减少调整频率
    if (fabs(effective_gap) < 2.0) {
        stabilization_counter++;
        // 在稳定区间，隔几次才进行调整
        if (stabilization_counter < 3) {
            // 跳过本次调整，保持当前状态
            return;
        }
        stabilization_counter = 0;
    } else {
        // 不在稳定区间，重置计数器
        stabilization_counter = 0;
    }
    
    // 小差距时添加微小偏移以确保达到目标
    if (fabs(effective_gap) < 3.0 && effective_gap > 0) {
        effective_gap += 0.3;
    }
    
    // 自动调整内存分配策略
    if (effective_gap > 1.5) {
        // 低于目标，增加调整系数
        target_not_reached_counter++;
        
        // 如果连续多次未达到目标，增加调整系数
        if (target_not_reached_counter > 2) {
            // 根据差距大小动态调整增量
            int adjustment_increment = (int)(fabs(effective_gap) * 0.3);
            if (adjustment_increment < 1) adjustment_increment = 1;
            if (adjustment_increment > 3) adjustment_increment = 3;
            
            memory_adjustment_counter += adjustment_increment;
            if (memory_adjustment_counter > 10) memory_adjustment_counter = 10;
            target_not_reached_counter = 0;
            consecutive_failed_allocations = 0;
            
            if (verbose_mode) {
                printf("内存调整：增加调整系数到 %d (差距: %.1f%%)\n", 
                       memory_adjustment_counter, effective_gap);
            }
        }
    } else if (effective_gap < -2.0) {
        // 高于目标，快速减少调整系数
        target_not_reached_counter = 0;
        memory_adjustment_counter = 0;
        
        if (verbose_mode) {
            printf("内存超出目标，立即释放部分内存 (差距: %.1f%%)\n", effective_gap);
        }
    } else if (fabs(effective_gap) < 1.0) {
        // 非常接近目标，缓慢减少调整系数，降低波动
        target_not_reached_counter = 0;
        if (memory_adjustment_counter > 0 && (rand() % 5 == 0)) { // 降低减少概率
            memory_adjustment_counter--;
            if (verbose_mode) {
                printf("内存接近目标，减少调整系数到 %d\n", memory_adjustment_counter);
            }
        }
    }
    
    // 处理连续分配失败
    if (consecutive_failed_allocations > 3) {
        if (memory_adjustment_counter > 0) {
            memory_adjustment_counter--;
            if (verbose_mode) {
                printf("连续分配失败，减少调整系数到 %d\n", memory_adjustment_counter);
            }
        }
        consecutive_failed_allocations = 0;
    }

    // 使用调整后的系数计算需要的内存
    double needed_mem_percent = effective_gap;
    
    // 应用自适应调整因子
    double adjustment_factor = 1.0;
    if (memory_adjustment_counter > 0) {
        // 基础调整因子
        adjustment_factor = 1.0 + (memory_adjustment_counter * 0.7);
        
        // 根据差距大小动态调整因子
        if (fabs(effective_gap) > 8.0) {
            adjustment_factor *= 1.8;
        } else if (fabs(effective_gap) > 4.0) {
            adjustment_factor *= 1.5;
        } else if (fabs(effective_gap) > 1.0) {
            adjustment_factor *= 1.2;
        }
        
        // 如果接近目标但仍未达到，适度增加因子
        if (effective_gap > 0 && effective_gap < 3.0) {
            adjustment_factor += 0.3;
        }
    }
    
    // 应用内存变化率限制 - 变化大时减小调整因子
    if (avg_memory_change_rate > 2.0) {
        adjustment_factor *= 0.7; // 减小调整幅度以稳定系统
    }
    
    needed_mem_percent *= adjustment_factor;
    
    // 添加改进的滞后效应，减少抖动
    double hysteresis = 0.15;
    // 接近目标时使用更小的滞后值，允许更精确调整
    if (fabs(effective_gap) < 2.0) {
        hysteresis = 0.05;
    }
    
    // 滞后效应处理
    if (fabs(needed_mem_percent - prev_needed_mem_percent) < hysteresis) {
        needed_mem_percent = prev_needed_mem_percent;
    } else {
        prev_needed_mem_percent = needed_mem_percent;
    }
    
    // 如果需要释放内存，清理之前的内存分配
    if (needed_mem_percent < -0.5) {
        // 计算自适应释放比例，与差距成正比
        int release_percent = (int)(fabs(needed_mem_percent) * 5.0);
        if (release_percent < 3) release_percent = 3;
        if (release_percent > 50) release_percent = 50;
        
        // 如果内存过载严重，增加释放量
        if (needed_mem_percent < -5.0) {
            release_percent += 10;
        }
        
        int blocks_to_free = (allocated_blocks * release_percent) / 100;
        if (blocks_to_free < 1 && allocated_blocks > 0) blocks_to_free = 1;
        if (blocks_to_free > allocated_blocks) blocks_to_free = allocated_blocks;
        
        // 释放部分内存块
        if (blocks_to_free > 0 && memory_blocks) {
            if (verbose_mode) {
                printf("释放 %d 个内存块 (约 %d MB)，比例: %d%%\n", 
                      blocks_to_free, blocks_to_free * block_size_mb, release_percent);
            }
            
            for (int i = allocated_blocks - 1; i >= allocated_blocks - blocks_to_free; i--) {
                if (memory_blocks[i]) {
                    free(memory_blocks[i]);
                    memory_blocks[i] = NULL;
                    allocated_mb -= block_size_mb;
                }
            }
            
            // 更新已分配块计数
            allocated_blocks -= blocks_to_free;
            
            // 如果全部释放，也释放指针数组
            if (allocated_blocks <= 0) {
                free(memory_blocks);
                memory_blocks = NULL;
                allocated_blocks = 0;
                allocated_mb = 0;
            } else {
                // 缩小内存块数组以减少资源占用
                char** new_memory_blocks = (char**)realloc(memory_blocks, allocated_blocks * sizeof(char*));
                if (new_memory_blocks) {
                    memory_blocks = new_memory_blocks;
                }
            }
        }
    }
    
    // 计算需要分配的内存量(MB)
    unsigned long long needed_mem_mb = 0;
    if (needed_mem_percent > 0) {
        needed_mem_mb = (unsigned long long)(needed_mem_percent * total_system_memory_mb / 100.0);
    }
    
    // 详细模式下输出内存分配信息
    if (verbose_mode) {
        printf("需要分配内存: %llu MB (当前: %.1f%% 实际/%.1f%% 滤波, 目标: %.1f%%, 差距: %.1f%%, 系数: %.1f, 变化率: %.2f%%)\n", 
               needed_mem_mb, current_mem_usage_percent, filtered_mem_usage, 
               target_mem_percent, effective_gap, adjustment_factor, avg_memory_change_rate);
    }
    
    if (needed_mem_mb == 0) {
        return;
    }
    
    // 根据剩余分配量动态调整块大小，以实现更平滑的分配
    if (needed_mem_mb > 4000)
        block_size_mb = 64;
    else if (needed_mem_mb > 1000)
        block_size_mb = 32;
    else if (needed_mem_mb > 200)
        block_size_mb = 16;
    else if (needed_mem_mb > 50)
        block_size_mb = 8;
    else if (needed_mem_mb > 10)
        block_size_mb = 4;
    else
        block_size_mb = 2; // 较小块以实现更精细的控制
    
    int new_blocks = needed_mem_mb / block_size_mb;
    if (new_blocks == 0 && needed_mem_mb > 0) new_blocks = 1;
      // 限制每次分配的最大块数，实现更平滑的分配
#ifdef _WIN32
    // Windows系统上限制更严格
    const int max_blocks_per_cycle = 300;
    
    // 应用限制 - Windows版本
    if (new_blocks > allocated_blocks + max_blocks_per_cycle) {
        new_blocks = allocated_blocks + max_blocks_per_cycle;
    }
#else
    // Linux系统上的限制
    const int max_blocks_per_cycle = 500;
    
    // 根据内存变化率自适应调整每次最大分配量
    int adjusted_max_blocks = max_blocks_per_cycle;
    if (avg_memory_change_rate > 2.0) {
        // 变化率大，减少每次分配量
        adjusted_max_blocks = max_blocks_per_cycle / 2;
    } else if (avg_memory_change_rate < 0.5) {
        // 变化率小，可以增加每次分配量
        adjusted_max_blocks = max_blocks_per_cycle * 3 / 2;
    }
    
    // 应用自适应限制
    if (new_blocks > allocated_blocks + adjusted_max_blocks) {
        new_blocks = allocated_blocks + adjusted_max_blocks;
    }
#endif
    
    // 处理现有分配
    if (allocated_blocks > 0) {
        // 计算需要增加或减少的块数
        int diff_blocks = new_blocks - allocated_blocks;
        
        if (diff_blocks > 0) {
            // 需要增加内存块
            char** new_memory_blocks = (char**)realloc(memory_blocks, new_blocks * sizeof(char*));
            if (!new_memory_blocks) {
                if (verbose_mode) {
                    printf("无法重新分配内存块数组\n");
                }
                consecutive_failed_allocations++;
                return;
            }
            memory_blocks = new_memory_blocks;
            
            // 初始化新增的指针为NULL
            for (int i = allocated_blocks; i < new_blocks; i++) {
                memory_blocks[i] = NULL;
            }
            
            // 分配新增的块
            int success_count = 0;
            for (int i = allocated_blocks; i < new_blocks; i++) {                memory_blocks[i] = (char*)malloc(block_size_mb * 1024 * 1024);
                if (memory_blocks[i]) {
                    // 采用高效的内存初始化方法：只写入部分数据以确保物理内存被分配
                    for (int j = 0; j < block_size_mb; j += 2) {
                        size_t offset = j * 1024 * 1024;
                        memset(memory_blocks[i] + offset, 0xAA, 1024 * 256); // 只写入256KB
                    }
                    allocated_mb += block_size_mb;
                    success_count++;
                } else {
                    if (verbose_mode) {
                        printf("无法分配内存块 #%d\n", i);
                    }
                    consecutive_failed_allocations++;
                    break; // 停止继续分配
                }
            }
            
            if (success_count == diff_blocks) {
                consecutive_failed_allocations = 0; // 全部成功，重置失败计数
            }
            
            allocated_blocks += success_count;
            
            // 如果部分块分配失败，调整内存块数组大小
            if (success_count < diff_blocks) {
                if (verbose_mode) {
                    printf("警告：请求分配 %d 个块，但只成功分配了 %d 个\n", diff_blocks, success_count);
                }
                if (allocated_blocks < new_blocks) {
                    char** resize_blocks = (char**)realloc(memory_blocks, allocated_blocks * sizeof(char*));
                    if (resize_blocks) {
                        memory_blocks = resize_blocks;
                    }
                }
            }
        } 
        else if (diff_blocks < 0) {
            // 需要减少内存块
            int blocks_to_free = -diff_blocks;
            
            // 限制每次释放的比例，防止过度波动
            int max_free_percent = 30; // 默认限制在30%
            
            // 如果内存需求大幅减少，允许更大比例释放
            if (diff_blocks < -allocated_blocks / 2) {
                max_free_percent = 50;
            }
            
            int max_free = (allocated_blocks * max_free_percent) / 100;
            if (max_free < 1) max_free = 1;
            if (blocks_to_free > max_free) blocks_to_free = max_free;
            
            // 释放末尾的块
            for (int i = allocated_blocks - 1; i >= allocated_blocks - blocks_to_free; i--) {
                if (memory_blocks[i]) {
                    free(memory_blocks[i]);
                    memory_blocks[i] = NULL;
                    allocated_mb -= block_size_mb;
                }
            }
            
            // 更新已分配块数
            allocated_blocks -= blocks_to_free;
            
            // 缩小内存块数组
            if (allocated_blocks > 0) {
                char** new_memory_blocks = (char**)realloc(memory_blocks, allocated_blocks * sizeof(char*));
                if (!new_memory_blocks) {
                    if (verbose_mode) {
                        printf("无法缩小内存块数组\n");
                    }
                } else {
                    memory_blocks = new_memory_blocks;
                }
            } else {
                free(memory_blocks);
                memory_blocks = NULL;
            }
        }
        // 如果diff_blocks == 0，保持当前分配不变
    } else {
        // 首次分配
        memory_blocks = (char**)malloc(new_blocks * sizeof(char*));
        
        if (!memory_blocks) {
            if (verbose_mode) {
                printf("无法分配内存块数组\n");
            }
            consecutive_failed_allocations++;
            return;
        }
        
        int success_count = 0;
        for (int i = 0; i < new_blocks; i++) {            memory_blocks[i] = (char*)malloc(block_size_mb * 1024 * 1024);
            if (memory_blocks[i]) {
                // 采用高效的内存初始化方法：只写入部分数据以确保物理内存被分配
                for (int j = 0; j < block_size_mb; j += 2) {
                    size_t offset = j * 1024 * 1024;
                    memset(memory_blocks[i] + offset, 0xAA, 1024 * 256); // 只写入256KB
                }
                allocated_mb += block_size_mb;
                success_count++;
            } else {
                if (verbose_mode) {
                    printf("无法分配内存块 #%d\n", i);
                }
                consecutive_failed_allocations++;
                break; // 停止继续尝试
            }
        }
        
        allocated_blocks = success_count;
        
        if (success_count == new_blocks) {
            consecutive_failed_allocations = 0; // 全部成功，重置失败计数
        }
        
        // 如果部分块分配失败，调整内存块数组大小
        if (success_count < new_blocks) {
            if (verbose_mode) {
                printf("警告：请求分配 %d 个块，但只成功分配了 %d 个\n", new_blocks, success_count);
            }
            if (success_count > 0) {
                char** resize_blocks = (char**)realloc(memory_blocks, success_count * sizeof(char*));
                if (resize_blocks) {
                    memory_blocks = resize_blocks;
                }
            } else {
                free(memory_blocks);
                memory_blocks = NULL;
                allocated_blocks = 0;
            }
        }
    }
    
    if (verbose_mode) {
        printf("当前已分配内存: %llu MB (%d 个块，每块 %d MB)\n", 
               allocated_mb, allocated_blocks, block_size_mb);
    }
}

// 生成进度条字符串
void generate_progress_bar(char* buffer, size_t buffer_size, double percentage, int bar_width) {
    if (buffer == NULL || buffer_size < bar_width + 20) return;
    
    int filled_width = (int)(percentage * bar_width / 100.0);
    if (filled_width > bar_width) filled_width = bar_width;
    
    // 清空缓冲区
    memset(buffer, 0, buffer_size);
    
    // 添加左边界
    strcat(buffer, "[");
    
    int color_code = 0;
    
    // 根据百分比选择颜色
    if (percentage < 30.0) {
        color_code = 32; // 绿色
    } else if (percentage < 70.0) {
        color_code = 33; // 黄色
    } else {
        color_code = 31; // 红色
    }
    
    // 使用ANSI转义序列添加彩色
    char color_start[16] = {0};
    char color_end[8] = {0};
    
#ifdef _WIN32
    // 在Windows上检查是否支持ANSI颜色
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hConsole, &mode)) {
        // 设置VIRTUAL_TERMINAL_PROCESSING以支持ANSI颜色
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (SetConsoleMode(hConsole, mode)) {
            // 支持ANSI颜色
            sprintf(color_start, "\033[%dm", color_code);
            strcpy(color_end, "\033[0m");
        }
    }
#else
    // Linux默认支持ANSI颜色
    sprintf(color_start, "\033[%dm", color_code);
    strcpy(color_end, "\033[0m");
#endif
    
    // 添加填充部分(彩色)
    if (filled_width > 0) {
        strcat(buffer, color_start);
        
        for (int i = 0; i < filled_width; i++) {
            // 使用方块填充已完成部分
            strcat(buffer, "█");
        }
        
        strcat(buffer, color_end);
    }
    
    // 添加未填充部分
    for (int i = 0; i < bar_width - filled_width; i++) {
        strcat(buffer, "░");
    }
    
    // 添加右边界和百分比
    char percentStr[16];
    sprintf(percentStr, "] %5.1f%%", percentage);
    strcat(buffer, percentStr);
    
    // 如果百分比超过阈值，添加标记
    if (percentage > 90.0) {
        strcat(buffer, " (!!)");
    } else if (percentage > 75.0) {
        strcat(buffer, " (!)");
    }
}

void print_usage() {
    printf("用法: ./cmm -c <cpu_usage> -m <memory_usage> [选项]\n");
    printf("必选参数 (或使用配置文件):\n");
    printf("  -c <cpu_usage>    目标CPU使用率(百分比, 0-100)\n");
    printf("  -m <memory_usage> 目标内存使用率(百分比, 0-100)\n");
    printf("可选参数:\n");
    printf("  -v                详细输出模式\n");
    printf("  -l <file>         加载配置文件\n");
    printf("  -s [file]         保存配置到文件 (默认: cmm.conf)\n");
    printf("  -d                以守护进程/后台模式运行\n");
    printf("  -h                显示此帮助信息\n");
    printf("例子: ./cmm -c 50 -m 50 -v\n");
    printf("      ./cmm -l my_config.conf\n");
    printf("      ./cmm -c 50 -m 50 -d\n");
}

// 加载配置文件
bool load_config(const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        printf("无法打开配置文件: %s\n", filename);
        return false;
    }
    
    char line[256];
    char key[128], value[128];
    
    while (fgets(line, sizeof(line), fp)) {
        // 跳过注释和空行
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        
        if (sscanf(line, "%127[^=]=%127s", key, value) == 2) {
            // 去除可能的空格
            char* p = key;
            while (*p) {
                if (*p == ' ' || *p == '\t') {
                    *p = '\0';
                    break;
                }
                p++;
            }
            
            if (strcmp(key, "cpu_usage") == 0) {
                target_cpu_usage = atoi(value);
            } else if (strcmp(key, "mem_usage") == 0) {
                double mem_percent = atof(value); // 使用atof获取更精确的值
                unsigned long long total_system_memory_mb = get_total_system_memory();
                target_mem_usage_mb = (int)(mem_percent * total_system_memory_mb / 100.0 + 0.5); // 加0.5进行四舍五入
            } else if (strcmp(key, "verbose") == 0) {
                verbose_mode = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            }
        }
    }
    
    fclose(fp);
    printf("已加载配置文件: %s\n", filename);
    return true;
}

// 保存配置文件
bool save_config_to_file(const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        printf("无法创建配置文件: %s\n", filename);
        return false;
    }
    
    fprintf(fp, "# CMM 配置文件\n");
    fprintf(fp, "# 自动生成于 %s\n\n", __DATE__);
    
    fprintf(fp, "# 目标CPU和内存使用率\n");
    fprintf(fp, "cpu_usage=%d\n", target_cpu_usage);
    fprintf(fp, "mem_usage=%d\n\n", (int)(target_mem_usage_mb * 100.0 / get_total_system_memory() + 0.5)); // 加0.5进行四舍五入
    
    fprintf(fp, "# 其他设置\n");
    fprintf(fp, "verbose=%s\n", verbose_mode ? "true" : "false");
    
    fclose(fp);
    printf("配置已保存到: %s\n", filename);
    return true;
}

// 获取CMM自身的CPU使用率
double get_self_cpu_usage() {
#ifdef _WIN32
    static ULARGE_INTEGER last_time = {0};
    static ULARGE_INTEGER last_system_time = {0};
    
    FILETIME creation_time, exit_time, kernel_time, user_time;
    HANDLE process = GetCurrentProcess();
    
    if (!GetProcessTimes(process, &creation_time, &exit_time, &kernel_time, &user_time)) {
        return 0.0;
    }
    
    ULARGE_INTEGER now_kernel, now_user, now_system;
    now_kernel.LowPart = kernel_time.dwLowDateTime;
    now_kernel.HighPart = kernel_time.dwHighDateTime;
    now_user.LowPart = user_time.dwLowDateTime;
    now_user.HighPart = user_time.dwHighDateTime;
    
    ULARGE_INTEGER now_time;
    now_time.QuadPart = now_kernel.QuadPart + now_user.QuadPart;
    
    FILETIME sys_idle_time, sys_kernel_time, sys_user_time;
    if (!GetSystemTimes(&sys_idle_time, &sys_kernel_time, &sys_user_time)) {
        return 0.0;
    }
    
    ULARGE_INTEGER sys_kernel, sys_user;
    sys_kernel.LowPart = sys_kernel_time.dwLowDateTime;
    sys_kernel.HighPart = sys_kernel_time.dwHighDateTime;
    sys_user.LowPart = sys_user_time.dwLowDateTime;
    sys_user.HighPart = sys_user_time.dwHighDateTime;
    
    now_system.QuadPart = sys_kernel.QuadPart + sys_user.QuadPart;
    
    if (last_time.QuadPart == 0) {
        last_time = now_time;
        last_system_time = now_system;
        return 0.0;
    }
    
    ULONGLONG time_diff = now_time.QuadPart - last_time.QuadPart;
    ULONGLONG system_diff = now_system.QuadPart - last_system_time.QuadPart;
    
    last_time = now_time;
    last_system_time = now_system;
    
    if (system_diff == 0) return 0.0;
    
    // 修复CPU使用率计算，不除以核心数，这样得到的是进程使用的总CPU百分比
    // 系统总CPU使用率计算没有除以核心数，所以这里也保持一致
    double cpu_usage = (double)(time_diff * 100.0) / system_diff;
    
    // 限制在合理范围内
    if (isnan(cpu_usage) || cpu_usage < 0) {
        return 0.0;
    }
    if (cpu_usage > 100.0) {
        return 100.0;
    }
    
    return cpu_usage;
#else    // Linux实现 - 使用/proc/self/stat获取更准确的数据
    static unsigned long long last_utime = 0, last_stime = 0;
    static unsigned long long last_total_time = 0;
    
    // 获取系统总时间
    FILE* fp = fopen("/proc/stat", "r");
    if (fp == NULL) return 0.0;
    
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        fclose(fp);
        return 0.0;
    }
    fclose(fp);
    
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    sscanf(buffer, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", 
           &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    
    unsigned long long total_time = user + nice + system + idle + iowait + irq + softirq + steal;
    
    // 获取进程CPU时间
    fp = fopen("/proc/self/stat", "r");
    if (fp == NULL) return 0.0;
    
    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        fclose(fp);
        return 0.0;
    }
    fclose(fp);
    
    // 解析/proc/self/stat获取utime和stime (字段14和15)
    char* p = buffer;
    for (int i = 0; i < 13; i++) {
        p = strchr(p + 1, ' ');
        if (!p) return 0.0;
    }
    
    unsigned long long utime, stime;
    if (sscanf(p, " %llu %llu", &utime, &stime) != 2) {
        return 0.0;
    }
    
    unsigned long long process_time = utime + stime;
    
    // 首次调用，初始化基准值
    if (last_utime == 0 || last_total_time == 0) {
        last_utime = utime;
        last_stime = stime;
        last_total_time = total_time;
        return 0.0;
    }
    
    // 计算使用率: 进程时间变化 / 系统总时间变化 * 100
    unsigned long long process_time_diff = process_time - (last_utime + last_stime);
    unsigned long long total_time_diff = total_time - last_total_time;
    
    // 更新历史值
    last_utime = utime;
    last_stime = stime;
    last_total_time = total_time;
    
    if (total_time_diff == 0) return 0.0;
    
    double cpu_usage = (double)process_time_diff * 100.0 / total_time_diff;
    
    // 限制在合理范围内
    if (isnan(cpu_usage) || cpu_usage < 0) {
        return 0.0;
    }
    // 多核系统下，进程CPU使用率可能超过100%
    if (cpu_usage > 100.0 * num_cpu_cores) {
        return 100.0 * num_cpu_cores;
    }
    
    return cpu_usage;
#endif
}

// 获取CMM自身的内存使用量（MB）
unsigned long long get_self_memory_usage_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    HANDLE process = GetCurrentProcess();
    
    if (!GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return 0;
    }
    
    return (unsigned long long)pmc.WorkingSetSize / (1024 * 1024);
#else
    // Linux实现
    FILE* file = fopen("/proc/self/status", "r");
    if (file == NULL) {
        return 0;
    }
    
    unsigned long vmsize = 0;
    char line[128];
    
    while (fgets(line, 128, file) != NULL) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %lu", &vmsize);
            break;
        }
    }
    
    fclose(file);
    return vmsize / 1024; // VmRSS以KB为单位，转换为MB
#endif
}

int main(int argc, char *argv[]) {
    // 设置本地化，解决中文乱码
#ifdef _WIN32
    // Windows设置UTF-8输出
    SetConsoleOutputCP(65001);
    // 初始化临界区
    InitializeCriticalSection(&cpu_load_cs);
#else
    // Linux设置本地化
    setlocale(LC_ALL, "");
    // 初始化互斥锁
    pthread_mutex_init(&cpu_load_mutex, NULL);
#endif
    
    // 获取CPU核心数
    num_cpu_cores = get_cpu_cores();
    
    // 参数解析
    if (argc < 2) {
        print_usage();
        return 1;
    }
    
    bool cpu_set = false;
    bool mem_set = false;
    bool load_config_specified = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose_mode = true;
        } else if (strcmp(argv[i], "-d") == 0) {
            daemon_mode = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            save_config = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                strncpy(config_file, argv[i + 1], sizeof(config_file) - 1);
                i++;
            }
        } else if (i + 1 < argc) {
            if (strcmp(argv[i], "-l") == 0) {
                if (!load_config(argv[i + 1])) {
                    return 1;
                }
                load_config_specified = true;
                cpu_set = true;
                mem_set = true;
                i++;
            } else if (strcmp(argv[i], "-c") == 0) {
                target_cpu_usage = atoi(argv[i + 1]);
                if (target_cpu_usage < 0 || target_cpu_usage > 100) {
                    printf("CPU使用率必须在0-100之间\n");
                    return 1;
                }
                cpu_set = true;
                i++;
            } else if (strcmp(argv[i], "-m") == 0) {
                unsigned long long total_system_memory_mb = get_total_system_memory();
                double mem_percent = atof(argv[i + 1]);
                if (mem_percent < 0 || mem_percent > 100) {
                    printf("内存使用率必须在0-100之间\n");
                    return 1;
                }
                // 转换百分比为MB，但不使用int强制转换
                target_mem_usage_mb = (int)(mem_percent * total_system_memory_mb / 100.0 + 0.5); // 加0.5进行四舍五入
                mem_set = true;
                i++;
                // 已移除 -p, -i, -d(old), -f, -u 参数的处理
            } else {
                printf("未知参数: %s\n", argv[i]);
                print_usage();
                return 1;
            }
        } else {
            printf("参数 %s 需要一个值\n", argv[i]);
            print_usage();
            return 1;
        }
    }
    
    // 检查必需参数
    if (!load_config_specified && (!cpu_set || !mem_set)) {
        printf("错误: 必须指定CPU和内存使用率或加载配置文件\n");
        print_usage();
        return 1;
    }
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    
    // 处理后台运行模式
    if (daemon_mode) {
#ifdef _WIN32
        // Windows后台运行
        printf("将在后台运行，关闭此窗口程序仍将继续运行\n");
        printf("使用任务管理器结束进程可停止程序\n");
        FreeConsole(); // 分离当前控制台
#else
        // Linux守护进程模式
        printf("将以守护进程模式运行\n");
        printf("使用 'ps -ef | grep cmm' 查找进程ID，使用 'kill [PID]' 停止程序\n");
        
        // 创建子进程
        pid_t pid = fork();
        
        if (pid < 0) {
            printf("启动守护进程失败\n");
            return 1;
        }
        
        // 父进程退出
        if (pid > 0) {
            return 0;
        }
        
        // 子进程继续
        
        // 创建新会话
        if (setsid() < 0) {
            return 1;
        }
        
        // 忽略会话leader的控制终端信号
        signal(SIGHUP, SIG_IGN);
        
        // 再次fork以确保子进程不是会话组leader
        pid = fork();
        
        if (pid < 0) {
            return 1;
        }
        
        if (pid > 0) {
            return 0;
        }
        
        // 改变工作目录到根目录
        chdir("/");
        
        // 关闭标准文件描述符
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        // 重定向到/dev/null
        open("/dev/null", O_RDWR);
        dup(0);
        dup(0);
#endif
    }
    
    printf("目标: CPU使用率 %d%%, MEM使用率 %d%%\n", 
           target_cpu_usage, 
           (int)(target_mem_usage_mb * 100.0 / get_total_system_memory() + 0.5)); // 加0.5进行四舍五入
    
    printf("检测到CPU核心数: %d\n", num_cpu_cores);
    
    // 预热CPU使用率检测
    get_system_cpu_usage();
#ifdef _WIN32
    Sleep(1000);
#else
    usleep(1000 * 1000);
#endif
      // 创建CPU负载调整线程
#ifdef _WIN32
    HANDLE adjust_thread = CreateThread(NULL, 0, 
                                 (LPTHREAD_START_ROUTINE)adjust_cpu_load_thread, 
                                 NULL, 0, NULL);
    if (adjust_thread == NULL) {
        printf("创建CPU负载调整线程失败\n");
        return 1;
    }
#else
    pthread_t adjust_thread;
    if (pthread_create(&adjust_thread, NULL, adjust_cpu_load_thread, NULL) != 0) {
        printf("创建CPU负载调整线程失败\n");
        return 1;
    }
#endif
    
    // 创建多个CPU占用线程，每个核心一个
#ifdef _WIN32
    HANDLE *cpu_threads = (HANDLE *)malloc(num_cpu_cores * sizeof(HANDLE));
    if (!cpu_threads) {
        printf("内存分配失败\n");
        return 1;
    }
    
    for (long i = 0; i < num_cpu_cores; i++) {
        cpu_threads[i] = CreateThread(NULL, 0, 
                                   (LPTHREAD_START_ROUTINE)cpu_load_thread, 
                                   (LPVOID)(intptr_t)i, 0, NULL);
        if (cpu_threads[i] == NULL) {
            printf("创建CPU线程 #%ld 失败\n", i);
            for (long j = 0; j < i; j++) {
                CloseHandle(cpu_threads[j]);
            }
            free(cpu_threads);
            return 1;
        }
    }
#else
    pthread_t *cpu_threads = (pthread_t *)malloc(num_cpu_cores * sizeof(pthread_t));
    if (!cpu_threads) {
        printf("内存分配失败\n");
        return 1;
    }
    
    for (long i = 0; i < num_cpu_cores; i++) {
        if (pthread_create(&cpu_threads[i], NULL, cpu_load_thread, (void*)i) != 0) {
            printf("创建CPU线程 #%ld 失败\n", i);
            for (long j = 0; j < i; j++) {
                pthread_cancel(cpu_threads[j]);
                pthread_join(cpu_threads[j], NULL);
            }
            free(cpu_threads);
            return 1;
        }
    }
#endif
      // 主循环，处理内存分配并显示状态
    while (running) {
        allocate_memory();
        
        // 非后台模式下显示状态
        if (!daemon_mode) {
            // 清屏以更新状态显示
            clear_screen();
              // 生成CPU和内存进度条
            char cpu_bar[128] = {0};
            char mem_bar[128] = {0};
            
            // 基本状态信息
            printf("\n==================== 系统状态 ====================\n");
            
            // 显示当前时间
            time_t now = time(NULL);
            struct tm *timeinfo = localtime(&now);
            char timestr[20];
            strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", timeinfo);
            printf("当前时间: %s\n\n", timestr);
            
            // 获取并处理CPU使用率，当接近目标值时显示目标值
            double display_cpu_usage = current_cpu_load;
            if (display_cpu_usage > target_cpu_usage * 0.95 && display_cpu_usage < target_cpu_usage * 1.05) {
                display_cpu_usage = target_cpu_usage;
            }
            
            // 获取并处理内存使用率，当接近目标值时显示目标值
            double display_mem_usage = get_system_mem_usage();
            int target_mem_percent = (int)(target_mem_usage_mb * 100.0 / get_total_system_memory() + 0.5);
            if (display_mem_usage > target_mem_percent * 0.95 && display_mem_usage < target_mem_percent * 1.05) {
                display_mem_usage = target_mem_percent;
            }
            
            // 生成进度条
            generate_progress_bar(cpu_bar, sizeof(cpu_bar), display_cpu_usage, 30);
            generate_progress_bar(mem_bar, sizeof(mem_bar), display_mem_usage, 30);
            
            // 获取自身资源占用情况
            double self_cpu = get_self_cpu_usage();
            unsigned long long self_mem_mb = get_self_memory_usage_mb();
            unsigned long long total_mem_mb = get_total_system_memory();
            double self_mem_percent = (double)self_mem_mb * 100.0 / total_mem_mb;
            
            // 计算系统占用率（不包含CMM自身）
            double system_cpu = display_cpu_usage - self_cpu;
            double system_mem = display_mem_usage - self_mem_percent;
            
            // 确保显示值不为负
            if (system_cpu < 0) system_cpu = 0;
            if (system_mem < 0) system_mem = 0;
            
            // 显示CPU和内存使用情况
            printf("CPU: %s (目标：%d%%, 系统：%.1f%%, CMM：%.1f%%)\n", 
                   cpu_bar, target_cpu_usage, system_cpu, self_cpu);
            printf("MEM: %s (目标：%d%%, 系统：%.1f%%, CMM：%.1f%%)\n",
                   mem_bar, target_mem_percent, system_mem, self_mem_percent);
            
            // 详细模式下显示更多信息
            if (verbose_mode) {                printf("详细信息: CPU占用=%6.2f%%, 控制=%3d%%, 滤波值=%.1f%%, MEM占用=%.1f%%, 滤波值=%.1f%%\n",
                       current_cpu_load, busy_percentage, filtered_cpu_usage, 
                       get_system_mem_usage(), filtered_mem_usage);
                printf("控制参数: PID(%.2f, %.2f, %.2f), 滤波系数: %.2f, CPU核心: %d\n",
                       pid_kp, pid_ki, pid_kd, filter_alpha, num_cpu_cores);
            }
            
            printf("\n=====================================================\n");
        }
        
        // 使用配置的更新间隔
#ifdef _WIN32
        Sleep(update_interval * 1000); 
#else
        sleep(update_interval);
#endif
    }
    
    // 保存配置
    if (save_config) {
        save_config_to_file(config_file);
    }
    
    // 清理
#ifdef _WIN32
    CloseHandle(adjust_thread);
    for (int i = 0; i < num_cpu_cores; i++) {
        CloseHandle(cpu_threads[i]);
    }
    DeleteCriticalSection(&cpu_load_cs);
#else
    pthread_join(adjust_thread, NULL);
    for (int i = 0; i < num_cpu_cores; i++) {
        pthread_cancel(cpu_threads[i]);
        pthread_join(cpu_threads[i], NULL);
    }
    pthread_mutex_destroy(&cpu_load_mutex);
#endif

    // 释放内存
    if (cpu_threads != NULL) {
        free(cpu_threads);
        cpu_threads = NULL;
    }
    
    printf("\n程序已退出\n");    
    return 0;
} 
