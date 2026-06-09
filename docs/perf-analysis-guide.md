# Linux 性能分析与优化实战指南

> 适用环境：Ubuntu 24.04（物理机或虚拟机均可，建议 4+ CPU / 8GB+ RAM）。
> 前置条件：已完成 [编译与测试指南](build_and_test_guide.md) 中的环境安装，能够正常编译运行 Hical。

---

## 目录

- [零、工具安装](#零工具安装)
- [一、perf stat：硬件计数器分析](#一perf-stat硬件计数器分析)
- [二、perf record + 火焰图：CPU 热点定位](#二perf-record--火焰图cpu-热点定位)
- [三、Heaptrack：内存分配分析](#三heaptrack内存分配分析)
- [四、缓存层次与 cache line](#四缓存层次与-cache-line)
- [五、实战：Hical 性能分析全流程](#五实战hical-性能分析全流程)
- [六、速查卡](#六速查卡)

---

## 零、工具安装

### 0.1 一键安装所有性能工具

```bash
# perf（必须匹配内核版本）
sudo apt install -y linux-tools-$(uname -r) linux-tools-generic

# heaptrack（内存分配分析）
sudo apt install -y heaptrack heaptrack-gui

# FlameGraph（火焰图生成脚本）
git clone --depth 1 https://github.com/brendangregg/FlameGraph.git ~/FlameGraph

# 辅助工具
sudo apt install -y valgrind strace sysstat hwloc
```

### 0.2 内核参数调整（perf / heaptrack 权限）

```bash
# ── perf 权限 ──
# 查看当前值（默认通常是 4，限制很严）
cat /proc/sys/kernel/perf_event_paranoid

# 临时放开（重启失效）
sudo sysctl -w kernel.perf_event_paranoid=-1
sudo sysctl -w kernel.kptr_restrict=0

# ── ptrace 权限（heaptrack --pid 运行时附着需要） ──
# 查看当前值（默认 1，禁止非父进程 ptrace）
cat /proc/sys/kernel/yama/ptrace_scope

# 临时放开（重启失效）
sudo sysctl -w kernel.yama.ptrace_scope=0

# ── 永久生效（写入配置文件） ──
cat << 'EOF' | sudo tee /etc/sysctl.d/99-perf.conf
kernel.perf_event_paranoid = -1
kernel.kptr_restrict = 0
kernel.yama.ptrace_scope = 0
EOF
sudo sysctl --system
```

**各级别含义**：

| 参数                        | 值                                                  | 权限范围 |
| --------------------------- | --------------------------------------------------- | -------- |
| `perf_event_paranoid` = -1  | 不限制（允许 tracepoint、CPU 事件、内核符号）       |
| `perf_event_paranoid` = 0   | 允许所有用户使用 CPU 事件                           |
| `perf_event_paranoid` = 1   | 仅允许非特权用户使用无内核态的 CPU 事件             |
| `perf_event_paranoid` = 2   | 仅允许用户态事件                                    |
| `perf_event_paranoid` = 3/4 | 禁止非 root 使用 perf                               |
| `yama.ptrace_scope` = 0     | 允许任意进程 ptrace（heaptrack --pid 需要）         |
| `yama.ptrace_scope` = 1     | 仅允许父进程 ptrace（默认值，阻止 heaptrack --pid） |

### 0.3 验证安装

```bash
perf --version          # perf version 6.x
heaptrack --version     # heaptrack 1.x
lstopo --version        # hwloc 2.x（查看 CPU 拓扑）
```

### 0.4 VirtualBox 的 PMU 限制（重要）

**实测结论**：当前 VirtualBox VM 的 `perf list hw` 输出为空——**所有硬件 PMU 事件均不可用**。这意味着 `cycles`、`instructions`、`cache-misses`、`branch-misses` 等硬件计数器全部无法使用。

| 计数器类型                                      | VirtualBox 支持 | 实测状态             |
| ----------------------------------------------- | --------------- | -------------------- |
| 软件事件（`task-clock`, `context-switches`）    | **完全可用**    | 内核统计，不依赖 PMU |
| 通用硬件事件（`cycles`, `instructions`）        | 不可用          | `perf list hw` 为空  |
| 精确硬件事件（`cache-misses`, `branch-misses`） | 不可用          | `perf list hw` 为空  |
| PEBS/LBR（精确事件采样）                        | 不可用          | 需要裸机或 KVM 直通  |

**根本原因**：VirtualBox 默认不向 Guest 暴露宿主机的 PMU（Performance Monitoring Unit）。即使启用 `--nested-hw-virt on`，也仅在支持 nested VT-x 的 CPU 上部分生效，且 VirtualBox 的 PMU 虚拟化实现不完整。

**影响与应对**：

| 工具            | 影响                                           | 替代方案                                                                               |
| --------------- | ---------------------------------------------- | -------------------------------------------------------------------------------------- |
| `perf stat`     | 无法看 IPC / cache-misses / branch-misses      | 用软件事件（`task-clock`、`context-switches`、`page-faults`、`cpu-clock`）判断基本特征 |
| `perf record`   | 不能用 `-e cycles`，但 **`-e cpu-clock` 可用** | 用 `-e cpu-clock` 做 CPU 采样，火焰图完全正常                                          |
| `perf c2c`      | 完全不可用（依赖 HITM 事件）                   | 需迁移到裸机/KVM 做 false sharing 检测                                                 |
| `perf annotate` | 正常（基于 `perf record` 的采样数据）          | 无影响                                                                                 |
| Heaptrack       | 无影响（纯用户态拦截 malloc）                  | 无影响                                                                                 |

```bash
# 验证可用事件——只有软件事件
perf list sw

# VirtualBox 下可用的 perf stat 事件集
perf stat -e task-clock,context-switches,cpu-migrations,page-faults,\
cpu-clock,minor-faults,major-faults \
    -p $SERVER_PID -- sleep 30

# perf record 在 VirtualBox 下用 cpu-clock（默认即可）
perf record -g -F 999 -p $PID -- sleep 30
# 等价于 perf record -g -F 999 -e cpu-clock -p $PID -- sleep 30
```

> **想获得完整硬件计数器？** 两个选择：
> 1. **迁移到 KVM**（推荐）：`virt-install` + `--cpu host-passthrough`，PMU 完整暴露
> 2. **裸机运行**：直接在物理 Linux 机器上跑，所有计数器可用
>
> 但即使在 VirtualBox 下，`perf record` + 火焰图 + Heaptrack 已经覆盖 **80% 的性能分析场景**——CPU 热点定位和内存分配分析都不需要硬件计数器。

---

## 一、perf stat：硬件计数器分析

### 1.1 是什么

`perf stat` 运行一个程序（或附着到已运行的进程），在程序运行期间计数 CPU 硬件性能事件（cycle 数、指令数、缓存命中/失效、分支预测命中/失效等），在程序结束或 Ctrl+C 时输出**汇总统计报告**。

它回答的核心问题是：**程序的瓶颈在 CPU 计算还是内存访问？**

### 1.2 核心概念

#### 硬件性能计数器（PMC / PMU）

CPU 内部有一组硬件寄存器（Performance Monitoring Unit），专门在硬件层面计数各种微架构事件。它们 **零开销** 地伴随 CPU 执行自然递增——不是采样，不是打桩，是**硬件自动计数**。

常用事件分为三类：

| 类别         | 事件                          | 含义                         |
| ------------ | ----------------------------- | ---------------------------- |
| **执行效率** | `cycles`                      | CPU 时钟周期数（含停顿）     |
|              | `instructions`                | 执行的指令数                 |
|              | `IPC = instructions / cycles` | 每周期指令数（核心效率指标） |
| **缓存**     | `cache-references`            | 最后一级缓存（LLC）访问次数  |
|              | `cache-misses`                | LLC 未命中（需从内存取数据） |
|              | `L1-dcache-load-misses`       | L1 数据缓存加载失效          |
| **分支**     | `branches`                    | 分支指令总数                 |
|              | `branch-misses`               | 分支预测失败数               |

#### IPC（Instructions Per Cycle）：最重要的单一指标

```
IPC = instructions / cycles
```

| IPC 范围      | 含义                                 | 典型场景                        |
| ------------- | ------------------------------------ | ------------------------------- |
| **> 2.0**     | CPU 执行管线充分利用，计算效率高     | 紧凑数值计算循环                |
| **1.0 ~ 2.0** | 正常范围                             | 大多数 well-optimized 程序      |
| **0.5 ~ 1.0** | 有停顿，可能是内存访问或分支预测问题 | 指针追踪、哈希表查找            |
| **< 0.5**     | 严重停顿——**Memory-bound**           | 大数组随机访问、cache thrashing |

**为什么 IPC 这么重要？**

现代 CPU 是超标量（superscalar）的——一个周期可以发射 4~6 条指令。如果 IPC 远低于理论值，说明 CPU 大量时间在"等待"（等数据从内存到达、等分支结果确定），而不是在"计算"。这直接决定了你的优化方向：

- IPC 高 + CPU 使用率高 → **CPU-bound**：优化算法、减少指令数
- IPC 低 + CPU 使用率高 → **Memory-bound**：优化数据布局、减少 cache miss
- CPU 使用率低 → **I/O-bound**：优化磁盘/网络（不在本节讨论范围）

### 1.3 基本用法

> **VirtualBox 环境提醒**：当前 VM 无硬件 PMU 事件，以下命令均使用**软件事件**。
> 理论知识部分保留 IPC / cache-misses 等概念——在裸机或 KVM 上它们完全可用。

#### 前置：编译项目

`perf stat` 本身不要求调试符号，但需要有可执行的二进制。如果还没编译过，先编译：

```bash
cd ~/projects/Hical

# Release 即可（perf stat 只看计数器，不需要符号）
# 如果后面还要做火焰图，直接用 RelWithDebInfo 一步到位
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=gcc-14 \
    -DCMAKE_CXX_COMPILER=g++-14 \
    -DHICAL_BUILD_BENCH=ON

cmake --build build -j$(nproc)
```

> 如果只跑 `perf stat`（不做火焰图），`Release` 也行。但通常性能分析是一套流程走下来的，建议直接用 `RelWithDebInfo`。

#### 方式一：直接运行程序（适合短任务）

```bash
# 运行测试二进制，结束后自动输出统计
# VirtualBox 下默认只收集软件事件（task-clock 等）
perf stat ./build/tests/test_router
```

> **注意**：`perf stat` 默认会尝试采集硬件事件（cycles/instructions 等），
> VirtualBox 下它们会显示 `<not supported>`——这是正常的，软件事件（task-clock/page-faults 等）仍会正常输出。
>
> **短任务的局限**：单元测试通常亚秒级完成，此时 `page-faults` 和 `context-switches` 是**累计值**（不是每秒），
> 多数来自进程启动开销（动态库加载、堆初始化），不代表运行时行为。
> `perf stat` 真正有价值的场景是**长时间运行的服务器进程 + 持续压力**（方式二）。

#### 方式二：附着到已运行的进程（适合服务器，推荐）

```bash
# 启动 bench_server
./build/bench_server &
SERVER_PID=$!

# 附着 perf stat，采集 30 秒
# 同时在另一个终端施压：wrk -t4 -c100 -d30s http://127.0.0.1:8080/
perf stat -p $SERVER_PID -- sleep 30
```

#### 方式三：指定软件事件列表（VirtualBox 可用）

```bash
# VirtualBox 可用的完整软件事件集
perf stat -e task-clock,cpu-clock,context-switches,cpu-migrations,\
page-faults,minor-faults,major-faults \
    -p $SERVER_PID -- sleep 30
```

#### 方式四：指定硬件事件（裸机/KVM 环境）

```bash
# 需要硬件 PMU 支持——VirtualBox 下会报 <not supported>
perf stat -e cycles,instructions,cache-references,cache-misses,branches,branch-misses \
    -p $SERVER_PID -- sleep 30
```

### 1.4 输出解读

#### VirtualBox 下的实际输出（软件事件）

```
 Performance counter stats for process id '12345':

       30,127.45 msec  task-clock                #    7.896 CPUs utilized
          15,234       context-switches           #  505.6 /sec
              82       cpu-migrations             #    2.7 /sec
           3,456       page-faults                #  114.7 /sec
             321       major-faults               #   10.7 /sec
           3,135       minor-faults               #  104.0 /sec

      3.8148 seconds time elapsed
```

**逐行解读**：

| 行                         | 含义                             | 判断依据                                      |
| -------------------------- | -------------------------------- | --------------------------------------------- |
| `task-clock #7.896 CPUs`   | 程序使用了约 8 个 CPU 核心的时间 | **> 1.0 说明多核运转**，接近 8 说明 CPU 密集  |
| `context-switches 505/sec` | 每秒上下文切换次数               | < 1000/sec 正常；> 10000 说明锁竞争或频繁阻塞 |
| `cpu-migrations 2.7/sec`   | 进程在不同 CPU 核心间迁移        | > 100/sec 考虑 CPU affinity 绑核              |
| `page-faults 114/sec`      | 缺页异常总数                     | 稳态时应很低；启动期高是正常的                |
| `major-faults 10/sec`      | 需要磁盘 I/O 的缺页              | 稳态 > 0 说明内存不足或 mmap 频繁             |
| `minor-faults 104/sec`     | 仅需页表映射的缺页               | 对应堆分配后首次访问，正常                    |

#### 裸机/KVM 下的完整输出（含硬件事件）

```
 Performance counter stats for process id '12345':

       30,127.45 msec  task-clock                #    7.896 CPUs utilized
          15,234       context-switches           #  505.6 /sec
              82       cpu-migrations             #    2.7 /sec
           3,456       page-faults                #  114.7 /sec
  98,765,432,100       cycles                     #    3.28 GHz
  78,912,345,600       instructions               #    0.80  insn per cycle
  12,345,678,900       branches                   #  409.8 M/sec
     345,678,000       branch-misses              #    2.80% of all branches
   5,678,900,100       cache-references           #  188.5 M/sec
     234,567,000       cache-misses               #    4.13% of all cache refs

      3.8148 seconds time elapsed
```

**额外硬件事件的解读**：

| 行                                  | 含义                | 本例判断              |
| ----------------------------------- | ------------------- | --------------------- |
| `instructions #0.80 insn per cycle` | **IPC = 0.80**      | 偏低，有内存停顿      |
| `branch-misses 2.80%`               | 分支预测失败率 2.8% | 正常（< 5% 不需担心） |
| `cache-misses 4.13%`                | LLC 未命中率 4.13%  | 偏高，需进一步分析    |

### 1.5 判断瓶颈类型

#### VirtualBox 下的判断（仅软件事件）

没有 IPC 和 cache-misses，用以下替代方案：

```
                     perf stat 输出
                         │
                   task-clock 的 CPUs 值
                    /              \
               < 1.0              > 1.0
           (单核都没用满)       (多核运转)
                |                   │
        I/O-bound 或           看 context-switches
        锁等待                  /               \
         |                 < 1000/sec        > 10000/sec
    用 strace            正常的事件驱动      锁竞争 / 阻塞
    看阻塞在哪               │                    │
                      看 page-faults          用火焰图看
                      / (major-faults)        futex/mutex 占比
                   > 100/sec       ~0
                内存不足/        正常
               映射频繁           │
                              **去 perf record 看热点**
                              （火焰图不需要 PMU）
```

**关键洞察**：VirtualBox 下虽然无法直接判断 CPU-bound vs Memory-bound，但通过 `perf record` 火焰图可以间接判断：
- 火焰图中 `malloc`/`free`/`memcpy`/`memmove` 占比高 → 内存分配/拷贝密集
- 火焰图中用户态业务函数占比高 → 计算密集
- 火焰图中 `epoll_wait` 占比高 → I/O 等待

#### 裸机/KVM 下的判断（完整硬件事件）

```
                     perf stat 输出
                         │
                   task-clock 的 CPUs 值
                    /              \
               < 1.0              > 1.0
           (单核都没用满)       (多核运转)
                |                   |
         I/O-bound 或          看 IPC 值
         锁等待                /          \
                          > 1.0          < 0.5
                      CPU-bound      Memory-bound
                      优化算法       优化数据布局
                                         |
                                   看 cache-misses
                                   /            \
                              > 10%           < 5%
                          数据局部性差     可能是 TLB miss
                          重排数据结构     看 page-faults
```

### 1.6 进阶：分事件组计数（裸机/KVM）

> **VirtualBox 不需要此节**——软件事件不存在寄存器竞争。保留此节供迁移到裸机/KVM 后使用。

硬件 PMU 寄存器数量有限（Intel 通常 4~8 个通用计数器）。如果一次请求的事件数超过寄存器数，`perf stat` 会进行**多路复用**（multiplexing）——轮流采样各事件，通过统计推算总值。这会引入误差。

解决方法：用 `{}` 分组，确保同组事件同时计数：

```bash
# 将强相关的事件放在同一组（需硬件 PMU）
perf stat -e '{cycles,instructions}' \
          -e '{cache-references,cache-misses}' \
          -e '{branches,branch-misses}' \
          -p $SERVER_PID -- sleep 30
```

### 1.7 重复测量统计

单次测量有波动，用 `-r` 多次运行取平均和标准差：

```bash
# 运行 5 次 test_router，输出平均值和标准差
perf stat -r 5 ./build/tests/test_router
```

输出底部会显示：

```
   0.0234 +- 0.0012 seconds time elapsed  ( +-  5.13% )
```

---

## 二、perf record + 火焰图：CPU 热点定位

### 2.1 是什么

`perf stat` 告诉你"程序整体的瓶颈类型"，`perf record` + 火焰图告诉你"**具体哪个函数占了多少 CPU 时间**"。

原理：`perf record` 以固定频率（如每秒 999 次）中断 CPU，记录当时正在执行的函数和完整调用栈。采样结束后，出现次数最多的函数就是最耗 CPU 的**热点函数**。

火焰图是对这些采样数据的可视化——宽度代表采样占比，高度代表调用深度。

### 2.2 编译要求：保留调试符号

```bash
cd ~/projects/Hical

# RelWithDebInfo = -O2 + -g（保留优化的同时有完整符号）
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=gcc-14 \
    -DCMAKE_CXX_COMPILER=g++-14 \
    -DHICAL_BUILD_BENCH=ON

cmake --build build -j$(nproc)
```

**不同构建类型对 profiling 的影响**：

| 构建类型           | 编译选项       | 火焰图效果                     | 性能数据代表性                |
| ------------------ | -------------- | ------------------------------ | ----------------------------- |
| Debug              | `-O0 -g`       | 所有函数可见，无内联           | 差（与 Release 性能差距巨大） |
| **RelWithDebInfo** | **`-O2 -g`**   | **大部分函数可见，少量被内联** | **好（推荐 profiling 用）**   |
| Release            | `-O2 -DNDEBUG` | 很多函数被内联/消除            | 最好（但难看清细节）          |

> 如果 RelWithDebInfo 下某些函数被内联看不到，加 `-fno-inline` 临时禁用内联。

### 2.3 perf record 录制

> **好消息**：`perf record` 在 VirtualBox 下完全可用。默认使用 `cpu-clock` 软件事件做采样，不依赖硬件 PMU。火焰图质量不受影响。
>
> 以下全部在**同一个终端**完成（`perf record` 用 `&` 放后台，不占终端）。

```bash

# 杀掉所有残留的 bench_server
pkill -9 bench_server

# 确认已清理干净
pidof bench_server
# 应该无输出

# 重新启动
./build/bench_server &
SERVER_PID=$!

# 验证进程还活着
sleep 1 && kill -0 $SERVER_PID 2>/dev/null && echo "running" || echo "dead"

# 验证服务正常
curl -s http://127.0.0.1:8080/ && echo " OK"

# 开始录制：采样 999Hz，记录调用栈，持续 30 秒
sudo perf record -g -F 999 -p $SERVER_PID -o perf.data -- sleep 30 &
PERF_PID=$!

# 同时施压（与录制同步 30 秒）
wrk -t4 -c100 -d30s http://127.0.0.1:8080/

# 等待 perf 录制结束
wait $PERF_PID
```

**参数详解**：

| 参数           | 含义                           | 为什么这样选                                        |
| -------------- | ------------------------------ | --------------------------------------------------- |
| `-g`           | 记录完整调用栈（dwarf unwind） | 生成火焰图必须                                      |
| `-F 999`       | 每秒采样 999 次                | 避免与 1000Hz 内核时钟谐振锁步（harmonic lockstep） |
| `-p $PID`      | 仅采样指定进程                 | 排除 wrk 和系统噪声                                 |
| `-o perf.data` | 输出文件名                     | 默认也是 perf.data                                  |
| `-- sleep 30`  | 录制 30 秒后自动停止           | 与 wrk 压测时长匹配                                 |

### 2.4 perf report 快速查看（终端 TUI）

```bash
sudo perf report -i perf.data
```

进入交互式 TUI 界面：
- 上下箭头选择函数，Enter 展开调用栈
- `+` 展开/折叠函数的 caller/callee 树
- `a` 查看函数的逐行汇编（annotate）
- `q` 退出

`perf report` 适合快速确认热点，但不如火焰图直观。

### 2.5 生成火焰图

```bash
# 步骤 1：导出符号化调用栈文本
sudo perf script -i perf.data > perf_out.txt

# 步骤 2：折叠调用栈
~/FlameGraph/stackcollapse-perf.pl perf_out.txt > perf.folded

# 步骤 3：生成 SVG 火焰图
~/FlameGraph/flamegraph.pl \
    --title "Hical bench_server (wrk 4t/100c/30s)" \
    --width 1400 \
    perf.folded > flame.svg

# 拷贝到宿主机查看
cp flame.svg /mnt/hical_host/
```

在 Windows 浏览器中打开 `flame.svg`。

### 2.6 火焰图读图指南

#### 第一步：打开火焰图

```bash
# 已生成 flame.svg（见 2.5 节）
# 拷贝到宿主机共享目录
cp flame.svg /mnt/hical_host/
```

在 Windows 上用浏览器打开（**右键 → 打开方式 → Chrome/Edge/Firefox**，不要双击）。

打开后你会看到一个彩色的层叠矩形图。初次看可能不知道从哪下手——按以下流程操作。

#### 第二步：理解基本布局

```
┌─────────────────────── 火焰图整体布局 ───────────────────────────┐
│                                                                    │
│  顶部 ──→ 叶子函数（真正消耗 CPU 的地方）  ← 重点看这里         │
│            │                                                       │
│            │  越宽 = 占 CPU 时间越多                               │
│            │                                                       │
│  底部 ──→ 入口函数（bench_server / main）                         │
│                                                                    │
│  X 轴：不代表时间顺序！宽度 = 采样占比                            │
│  Y 轴：调用栈深度（谁调用了谁）                                    │
│  颜色：随机的，不代表任何含义（搜索高亮时除外）                    │
└────────────────────────────────────────────────────────────────────┘
```

**最关键的一点**：底部的函数块很宽，不代表它自身慢——它宽是因为它调用了很多子函数。**真正消耗 CPU 的是顶部那些最宽的、没有再往上长的块**（叶子函数）。

#### 第三步：鼠标悬停——看函数名和占比

把鼠标移到任意一个彩色矩形块上，**底部信息栏**会显示：

```
Function: __x64_sys_sendto (19,771,771,752 samples, 66.52%)
```

这告诉你：
- **函数名**：`__x64_sys_sendto`（发送数据的系统调用）
- **采样次数**：19,771,771,752
- **占比**：66.52%（这个函数及其所有子调用合计占总 CPU 时间的 66.52%）

> **注意**：占比是"包含子调用"的。如果只想看函数自身的开销（不含子调用），看它上面是否还有更窄的块叠着。

#### 第四步：从底部到顶部逐层查看

实操流程——以 Hical 的 `flame.svg` 为例：

**1. 找最底部最宽的块**

最底部应该是 `bench_server (100%)`——整个进程。往上一层是几个大块并排。

**2. 看第二层的大块分布**

你会看到几个并排的大块：
- `do_syscall_64 (69.68%)` — 系统调用路径（最宽！占了屏幕 2/3）
- `epoll_reactor::run (9.99%)` — Asio 事件循环
- `schedule (8.89%)` — 线程调度
- 其他较窄的块

**3. 顺着最宽的块往上追踪**

点击 `do_syscall_64` 放大，继续看它内部：
```
do_syscall_64 (69.68%)
  └── __x64_sys_sendto (66.52%)    ← 绝大部分是发送
      └── tcp_sendmsg (65.49%)
          └── __tcp_push_pending_frames (62.58%)
              └── tcp_write_xmit (62.40%)
                  └── ip_queue_xmit (60.35%)
                      └── ... 一路到 _raw_spin_unlock_irqrestore (48.46%)
```

**4. 找到最终的叶子函数**

追踪到顶部，最宽的叶子函数是 `_raw_spin_unlock_irqrestore (48.46%)`。这说明将近一半的 CPU 时间花在了内核自旋锁释放上——这是 VirtualBox 虚拟化的开销，用户态代码无法优化。

#### 第五步：点击放大（Zoom）

- **点击**任意函数块 → 以它为 100% 放大显示，只看它的子调用
- 放大后左上角出现 **"Reset Zoom"** 文字 → 点击回到全局视图
- 这在追踪深层调用栈时非常有用——比如点击 `handleSession` 放大后，就能看清 Hical 框架内部各函数的相对占比

#### 第六步：搜索特定函数

按键盘 **`s`** 键（或点击右上角 "Search"），输入关键词：

| 你想知道什么  | 搜索什么               | 看结果                       |
| ------------- | ---------------------- | ---------------------------- |
| 框架总开销    | `hical`                | 右下角显示百分比（实测 ~2%） |
| HTTP 解析开销 | `phr_parse`            | 实测 0.02%                   |
| 内存分配开销  | `malloc`               | 如果 > 5% 需要优化           |
| 网络发送开销  | `sendto\|sendmsg`      | 实测 66%（内核态，正常）     |
| 协程调度开销  | `awaitable\|coroutine` | 看协程帧分配/恢复            |
| Asio 调度     | `epoll\|reactor`       | 实测 ~10%                    |

搜索后：
- 匹配的块变为**紫红色高亮**
- 右下角显示 **"matched: X.X% of samples"**
- 按 **Esc** 清除高亮

#### 第七步：判断是否有问题

拿到上面的数据后，对照下表判断：

| 搜索结果                        | 正常   | 需要关注 | 说明                                   |
| ------------------------------- | ------ | -------- | -------------------------------------- |
| 搜 `hical`                      | < 5%   | > 10%    | 框架自身代码不应占太多 CPU             |
| 搜 `malloc\|free\|operator new` | < 2%   | > 5%     | 内存分配太频繁，考虑 PMR/预分配        |
| 搜 `memcpy\|memmove`            | < 3%   | > 5%     | 数据拷贝过多，考虑零拷贝               |
| 搜 `mutex\|futex`               | < 1%   | > 5%     | 锁竞争严重                             |
| 搜 `epoll_wait`                 | 5~30%  | > 50%    | I/O 等待过多（连接少或处理太快）       |
| 内核态总占比                    | 60~90% | —        | Hello World 场景正常，业务复杂时会降低 |

#### 实操总结：看火焰图的 5 步口诀

```
1. 悬停底部 → 确认进程名和总采样数
2. 找最宽块 → 从底往上追踪调用链
3. 看顶部叶子 → 那才是真正的热点
4. 搜 "hical" → 确认框架开销占比
5. 搜 "malloc" → 确认有无分配器压力
```

**Hical bench_server 实测火焰图热点分布**（Hello World 场景，wrk 4t/100c/30s，136K QPS）：

```
bench_server (100%)
│
├── 内核态 TCP/IP 协议栈 (~88%)
│   ├── __x64_sys_sendto (66.52%)          ← 发送响应的系统调用
│   │   └── tcp_sendmsg (65.49%)
│   │       └── __tcp_push_pending_frames (62.58%)
│   │           └── tcp_write_xmit (62.40%)
│   │               └── ip_queue_xmit → __ip_queue_xmit (60.35%)
│   │                   └── ip_output → ip_finish_output (57.64%)
│   │                       └── softirq 网络收发 (55%)
│   │                           └── _raw_spin_unlock_irqrestore (48.46%)  ← 自旋锁
│   │
│   ├── __x64_sys_recvfrom (6.54%)         ← 读取请求的系统调用
│   │   └── tcp_recvmsg_locked (2.23%)
│   │
│   └── schedule / finish_task_switch (8.9%) ← 线程调度/上下文切换
│
├── Boost.Asio 事件循环 (~10%)
│   ├── epoll_reactor::run (9.99%)         ← reactor 等待 + 分发事件
│   │   └── epoll_wait (9.83%)             ← 内核 epoll 等待
│   ├── reactive_socket_recv_op::do_perform (7.62%)  ← 读操作完成回调
│   └── scheduler::do_run_one (0.44%)      ← 协程恢复调度
│
├── Hical 用户态 (~2%)
│   ├── handleSession (1.27%)              ← 请求处理主循环
│   ├── writeResponse (0.41%)              ← 响应写入
│   ├── HeaderMap::set (0.60%)             ← 设置响应头
│   ├── Router::resolveRoute (0.23%)       ← 路由查找
│   ├── serializeHeadTo (0.07%)            ← 响应头序列化
│   ├── phr_parse_request (0.02%)          ← HTTP 解析
│   └── HttpRequest::fromParsed (0.03%)    ← 请求构造
│
└── 其他
    ├── clock_gettime (0.17%)              ← 时间戳获取
    ├── [unknown] (27.26% 底层)            ← VirtualBox 虚拟化层 / 缺失符号
    └── vbg_req_perform (0.33%)            ← VirtualBox Guest Additions 开销
```

**关键发现**：

1. **Hical 框架代码仅占 ~2%** — 说明框架效率极高，几乎所有 CPU 时间都在内核网络栈中
2. **内核 TCP 发送占绝对主导（66%）** — `sendto` → `tcp_sendmsg` → IP 协议栈 → 自旋锁释放。这是 loopback 网络的正常表现，不是代码问题
3. **`_raw_spin_unlock_irqrestore` 占 48%** — VirtualBox 虚拟化环境下自旋锁释放开销大（`kvm_kick_cpu` IPI 中断），裸机上这个比例会低很多
4. **`epoll_wait` 占 9.83%** — Asio 等待事件就绪，正常（说明 CPU 有余量，不是 100% 忙碌）
5. **`phr_parse_request` 仅 0.02%** — picohttpparser 零拷贝解析极快
6. **无 `malloc`/`free` 出现在热点中** — PMR 内存池和零拷贝设计生效，无分配器压力

> **结论**：在 Hello World 场景下，Hical 框架本身已无可优化空间。瓶颈在内核网络栈（loopback + VirtualBox 虚拟化开销）。如果要进一步提升 QPS，需要考虑：
> - 裸机运行（消除 VirtualBox 自旋锁开销）
> - io_uring 替代 sendto/recvfrom（减少系统调用次数）
> - SO_REUSEPORT 多 acceptor（已实现，Linux 下自动启用）

### 2.7 差异火焰图（A/B 对比）

优化前后各录一份 perf.data，生成差异火焰图：

```bash
# 假设已有两次录制
sudo perf script -i perf_baseline.data > baseline.txt
sudo perf script -i perf_optimized.data > optimized.txt

~/FlameGraph/stackcollapse-perf.pl baseline.txt > baseline.folded
~/FlameGraph/stackcollapse-perf.pl optimized.txt > optimized.folded

# 生成差异火焰图
~/FlameGraph/difffolded.pl baseline.folded optimized.folded \
    | ~/FlameGraph/flamegraph.pl \
    --title "Diff: baseline vs optimized" \
    > diff_flame.svg
```

**颜色含义**：
- **红色**：回归（该函数 CPU 占比**增加**了）
- **蓝色**：优化（该函数 CPU 占比**减少**了）

### 2.8 perf annotate：逐行/逐指令热点

当火焰图定位到某个函数很热后，用 `perf annotate` 看函数内部哪一行最热：

```bash
# 交互式查看（TUI）
sudo perf annotate -i perf.data -s phr_parse_request

# 或导出为文本
sudo perf annotate -i perf.data -s phr_parse_request --stdio > annotate_parse.txt
```

输出示例（左边数字是该行采样占比）：

```
       │     for (;;) {
       │         CHECK_EOF();
 12.34 │         ch = *buf;
       │         if (ch == '\r') {
  8.76 │             ++buf;
       │             EXPECT_CHAR('\n');
       │             break;
       │         }
 23.45 │         *token_start++ = ch;    ← 热点行：字节拷贝
       │         ++buf;
       │     }
```

---

## 三、Heaptrack：内存分配分析

### 3.1 是什么

Heaptrack 是一个**堆内存分配追踪器**。它记录程序运行期间每一次 `malloc`/`new`/`free`/`delete`，回答以下问题：

- 总共分配了多少次？多少字节？
- **哪个函数**分配最多（调用栈追踪）？
- 峰值内存使用（高水位线）在哪个时间点？
- 有没有内存泄漏（分配了但从未释放）？
- 临时分配（分配后很快释放）有多少？这些是优化目标。

### 3.2 与 Valgrind (Massif) 的对比

|                | Heaptrack                | Valgrind --tool=massif |
| -------------- | ------------------------ | ---------------------- |
| **性能开销**   | 2~5x 减速                | 20~50x 减速            |
| **数据粒度**   | 每次分配的完整调用栈     | 定期快照               |
| **GUI**        | heaptrack_gui（丰富）    | ms_print（文本）       |
| **适用场景**   | **日常内存分析（推荐）** | 极精确内存画像         |
| **调用栈深度** | 完整                     | 默认 12 层             |

### 3.3 基本用法

#### 方式一：启动时录制（推荐）

最可靠的方式——不依赖 ptrace 权限，heaptrack 直接包裹进程启动，注入成功率 100%。

需要**两个终端**（Tabby 开两个 Tab，或用 tmux 分屏）：

```bash
# ── 终端 1 ──

# 确保端口干净
pkill -9 bench_server

# heaptrack 直接包裹启动（会占住终端）
heaptrack ./build/bench_server
# 输出: heaptrack output will be written to "/home/hical/projects/Hical/heaptrack.bench_server.42147.zst"
# bench_server 开始监听，终端被占住
```

```bash
# ── 终端 2 ──

# 验证服务正常
curl -s http://127.0.0.1:8080/ && echo " OK"

# 施压 30 秒
wrk -t4 -c100 -d30s http://127.0.0.1:8080/

# 压测结束后回终端 1 按 Ctrl+C 停止 bench_server
# heaptrack 自动输出统计摘要和数据文件路径
```

终端 1 按 Ctrl+C 后会看到类似输出：

```
heaptrack stats:
        allocations:            1308297
        leaked allocations:     6
        temporary allocations:  622
```

#### 方式二：附着到已运行的进程

> **前置条件**：需要先放开 ptrace 限制，否则会报 `Cannot runtime-attach, you need to set /proc/sys/kernel/yama/ptrace_scope to 0`。
> 见 [0.2 节](#02-内核参数调整perf--heaptrack-权限) 的 `kernel.yama.ptrace_scope=0` 设置。
>
> **注意**：即使设置了 ptrace_scope=0，`--pid` 方式仍可能因注入时机问题导致数据为空。**优先使用方式一**。

需要**两个终端**（Tabby 开两个 Tab，或用 tmux 分屏）：

```bash
# ── 终端 1：启动 bench_server ──
./build/bench_server &
SERVER_PID=$!
echo "PID: $SERVER_PID"

# 附着 heaptrack（会占住终端，前台阻塞）
heaptrack --pid $SERVER_PID
# 压测结束后 Ctrl+C 停止录制
```

```bash
# ── 终端 2：施压 ──
wrk -t4 -c100 -d30s http://127.0.0.1:8080/
# 压测结束后回终端 1 按 Ctrl+C
```

### 3.4 分析结果

#### 方式一：GUI 分析（需要 X11 转发或桌面）

```bash
heaptrack_gui heaptrack.bench_server.42147.zst
```

GUI 提供以下视图：

| Tab                       | 显示内容                           | 关注点                 |
| ------------------------- | ---------------------------------- | ---------------------- |
| **Summary**               | 总分配次数/字节、峰值、泄漏量      | 全局概览               |
| **Bottom-Up**             | 按分配量排序的调用栈（从叶子到根） | 找到分配最多的函数     |
| **Top-Down**              | 从 main 向下展开的分配树           | 理解分配来源链路       |
| **Flame Graph**           | 分配量火焰图（类似 CPU 火焰图）    | 直观定位热点           |
| **Consumed**              | 时间线上的内存消耗曲线             | 看增长趋势和峰值       |
| **Allocations**           | 时间线上的分配次数曲线             | 找高频分配时段         |
| **Temporary Allocations** | 临时分配（分配后很快释放）         | **PMR 优化的首要目标** |

#### 方式二：文本分析（推荐，SSH 终端直接用）

`heaptrack_print` 的完整输出非常长（数千行），包含每个分配点的完整调用栈。实际使用时按需查看各段落：

```bash
FILE=heaptrack.bench_server.42147.zst

# ── 末尾总结统计（最常用，快速了解全貌） ──
heaptrack_print $FILE | tail -10

# ── 分配次数排名（MOST CALLS TO ALLOCATION FUNCTIONS） ──
# 每条含完整调用栈（10~15 行），head -60 约看前 4~5 条
heaptrack_print $FILE | sed -n '/^MOST CALLS TO ALLOCATION/,/^PEAK MEMORY/p' | head -60

# ── 峰值内存排名（PEAK MEMORY CONSUMERS） ──
heaptrack_print $FILE | sed -n '/^PEAK MEMORY CONSUMERS/,/^MOST TEMPORARY/p' | head -60

# ── 临时分配排名（MOST TEMPORARY ALLOCATIONS，优化重点） ──
heaptrack_print $FILE | sed -n '/^MOST TEMPORARY/,/^total runtime/p' | head -60

# ── 导出火焰图格式（生成可视化 SVG） ──
heaptrack_print --flamegraph-cost-type allocations $FILE > heap.folded
~/FlameGraph/flamegraph.pl \
    --title "Hical Heap Allocations" \
    --countname "allocs" \
    --colors mem \
    heap.folded > heap_flame.svg
```

**sed 命令说明**：`sed -n '/起始标记/,/结束标记/p'` 只打印两个标记之间的行，配合 `head -60` 截取前几条。heaptrack_print 输出结构固定为三大段：`MOST CALLS` → `PEAK MEMORY` → `MOST TEMPORARY` → `total runtime`，用相邻段的标题做切割边界。

### 3.5 解读 Heaptrack 输出

#### 总结统计（末尾）

`heaptrack_print ... | tail -20` 输出的总结统计是最重要的全局概览：

```
total runtime: 74.94s.
calls to allocation functions: 1308297 (17457/s)
temporary memory allocations: 3110 (41/s)
peak heap memory consumption: 2.14M
peak RSS (including heaptrack overhead): 10.64M
total memory leaked: 1.06K
```

**逐行解读**：

| 指标                  | 本例数据            | 含义                 | 判断                                                          |
| --------------------- | ------------------- | -------------------- | ------------------------------------------------------------- |
| calls to allocation   | 1,308,297 (17457/s) | 总堆分配次数和频率   | 17K/s 在 136K QPS 下 → 约 8 个请求才 1 次分配，PMR 池效果显著 |
| temporary allocations | 3,110 (41/s)        | 分配后很快释放的次数 | 仅占总分配的 0.24%，极低                                      |
| peak heap memory      | 2.14M               | 堆内存峰值           | 极低，框架本身内存占用小                                      |
| total memory leaked   | 1.06K               | 未释放的内存         | 1KB 泄漏 = 全局单例/静态对象，不是真正泄漏                    |

#### MOST CALLS TO ALLOCATION FUNCTIONS（分配次数排名）

这段按**分配次数**排序，告诉你哪个函数调用 malloc 最频繁：

```
751586 calls with 1.14M peak from
    boost::asio::aligned_new                     ← Asio 内部 handler 内存池
    └→ hical::HttpServer::start()

195994 calls with 61.15K peak from
    hical::HttpServer::handleSession             ← 协程帧分配（每请求 1 次）
    └→ HttpSessionImpl.cpp:767

195986 calls with 16.02K peak from
    boost::asio::async_write                     ← 响应写入的 handler 分配
    └→ hical::writeResponse (HttpSessionImpl.cpp:177)
```

**解读**：75 万次 `aligned_new` 是 Asio 的 handler 分配器——Asio 内部有 thread-local recycling 机制，实际不会每次都走系统 malloc。19.5 万次 handleSession 对应每个请求的协程帧分配。

#### PEAK MEMORY CONSUMERS（峰值内存排名）

这段按**峰值内存消耗**排序，告诉你哪个函数占内存最多：

```
1.14M  peak over 751586 calls from
    boost::asio::aligned_new                     ← Asio handler 池（最大单项）

698.54K consumed over 81 calls from
    hical::HttpServer::acceptLoop                ← 连接接受协程（81 个 = 连接数）
    └→ HttpServer.cpp:372

663.63K consumed over 81 calls from
    std::__cxx11::basic_string<>::resize()       ← readBuf 字符串缓冲区
    └→ HttpSessionImpl.cpp:305

61.15K consumed over 195994 calls from
    hical::HttpServer::handleSession             ← 请求处理协程帧
```

**解读**：峰值总共 2.14M，其中 Asio handler 池 1.14M，连接级缓冲区 698K + 663K。每个连接的 readBuf（`HttpSessionImpl.cpp:305`）约 8KB（663K / 81 连接），**这是测试时 81 条连接都处于活跃读写期的快照**；后引入 `ReadBufferPool` 借还后，空闲连接不持有 readBuf，百万空闲长连接场景下每连接节省约 7.5KB。

#### MOST TEMPORARY ALLOCATIONS（临时分配排名，优化重点）

这段是**最有价值的优化线索**——临时分配意味着"分配了又很快释放"，是 PMR 池可以消除的目标：

```
2382 temporary (0.86%) from
    std::__new_allocator<>::allocate              ← STL 容器内部分配
    └→ hical::HttpServer::handleSession (HttpSessionImpl.cpp:726)

696 temporary (0.09%) from
    boost::asio::aligned_new                     ← Asio handler 回收不及时

373 temporary (0.19%) from
    hical::HttpServer::handleSession             ← 协程帧
    └→ HttpSessionImpl.cpp:767
```

**解读**：总共只有 3,110 次临时分配（41 次/秒），在 136K QPS 下占比 < 1%。说明：
- Hical 的零拷贝 HTTP 解析（`string_view` 引用 `readBuf`）有效避免了请求级临时分配
- `FixedBuffer<4096>` 栈缓冲区避免了响应序列化的堆分配
- `ReadBufferPool` 借还 `readBuf`：每请求借一块，响应写完归还，同一缓冲区在连接间复用

### 3.6 与 Hical PMR 内存池配合分析

Hical 的三级 PMR 内存策略（`src/core/MemoryPool.h`）：

| 层级      | 作用域   | 分配器类型                     | 目标               |
| --------- | -------- | ------------------------------ | ------------------ |
| L1 全局池 | 进程级   | `synchronized_pool_resource`   | 跨线程共享对象     |
| L2 线程池 | 线程级   | `unsynchronized_pool_resource` | 无锁线程局部分配   |
| L3 请求池 | 单请求级 | `monotonic_buffer_resource`    | 请求结束一次性释放 |

**分析策略**：

1. 先跑 heaptrack 看临时分配热点
2. 如果热点是请求处理路径上的 `std::string` / `std::vector` → 改用 `PmrBuffer`
3. 如果热点是 `boost::json::value` → 使用 PMR-aware JSON（`boost::json::storage_ptr`）
4. 再次跑 heaptrack 验证临时分配减少

---

## 四、缓存层次与 cache line

### 4.1 为什么需要关心缓存

现代 CPU 和内存之间有巨大的速度鸿沟：

```
┌──────────┐
│ CPU 寄存器│ ~0.3 ns      (1 cycle)
├──────────┤
│ L1 Cache │ ~1 ns        (3-4 cycles)     32-48 KB / 核
├──────────┤
│ L2 Cache │ ~4 ns        (10-12 cycles)   256 KB-1 MB / 核
├──────────┤
│ L3 Cache │ ~12 ns       (30-40 cycles)   8-32 MB / 共享
├──────────┤
│ 主内存    │ ~60-100 ns  (150-300 cycles)  16 GB (VM)
├──────────┤
│ SSD      │ ~100,000 ns  (~300K cycles)
├──────────┤
│ 网络     │ ~500,000 ns
└──────────┘
```

**关键点**：L1 和主内存的延迟差 **100 倍**。一次 cache miss 相当于 CPU 空等 100~300 个周期——在这段时间里 CPU 本来可以执行几百条指令。

### 4.2 Cache Line：缓存的最小单位

CPU 缓存不是按字节读取的，而是按 **cache line**（缓存行）为单位。在 x86-64 架构上：

```
1 cache line = 64 字节
```

当 CPU 读取内存地址 `0x1000` 处的 1 个字节时，它实际上会把地址 `0x1000` ~ `0x103F`（64 字节对齐的整块）全部加载到缓存中。

**这意味着**：
- 连续访问的数据（如数组遍历）天然享受缓存预取的好处
- 随机跳跃访问的数据（如链表遍历、哈希表探测）每次可能触发 cache miss

### 4.3 查看 CPU 缓存拓扑

```bash
# 方式一：lstopo（来自 hwloc 包）
lstopo --no-io --of txt

# 方式二：直接读 sysfs
lscpu | grep -i cache

# 方式三：详细参数
getconf -a | grep CACHE
```

`lscpu` 典型输出（i7-11700K VirtualBox 8核）：

```
L1d cache:   384 KiB   (8 instances, 48 KiB/核)
L1i cache:   256 KiB   (8 instances, 32 KiB/核)
L2 cache:    4 MiB     (8 instances, 512 KiB/核)
L3 cache:    16 MiB    (1 instance, 共享)
```

### 4.4 缓存影响的分析方法

#### 裸机/KVM：用 perf stat 直接量化 cache miss

```bash
# 需要硬件 PMU 支持——VirtualBox 下不可用
perf stat -e L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses,\
dTLB-loads,dTLB-load-misses \
    -p $SERVER_PID -- sleep 30
```

输出解读：

| 指标                                    | 健康值 | 异常信号                   |
| --------------------------------------- | ------ | -------------------------- |
| L1-dcache-load-misses / L1-dcache-loads | < 5%   | > 10% 说明数据局部性差     |
| LLC-load-misses / LLC-loads             | < 5%   | > 20% 说明工作集超过 L3    |
| dTLB-load-misses / dTLB-loads           | < 0.5% | > 1% 考虑大页（hugepages） |

#### VirtualBox：间接推断缓存影响

无硬件 PMU 不代表无法分析缓存——用以下四种替代方法。

---

**方法一：perf stat 的 page-faults + major-faults**

最简单的间接指标。page fault 虽然和 cache miss 不是一回事，但 major-faults（需要磁盘换页）比 cache miss 更严重，能检测出内存压力问题。

```bash
# ── 操作步骤 ──

# 1. 启动 bench_server
./build/bench_server &
SERVER_PID=$!

# 2. 采集 30 秒（同时在另一个终端施压）
perf stat -e page-faults,major-faults,minor-faults \
    -p $SERVER_PID -- sleep 30

# 3. 另一个终端同步施压
wrk -t4 -c100 -d30s http://127.0.0.1:8080/

# 4. 等待 perf stat 输出结果
```

**判断标准**：

| 指标               | 稳态健康值 | 异常信号                                              |
| ------------------ | ---------- | ----------------------------------------------------- |
| `major-faults`     | 0          | > 0 → 内存不足导致换页，严重性能问题                  |
| `minor-faults`     | < 100/sec  | > 1000/sec → 频繁新内存分配（每次新页面首次访问触发） |
| `page-faults` 总量 | 稳定不增长 | 持续增长 → 内存泄漏或工作集膨胀                       |

> 之前实测 bench_server 在 136K QPS 下仅 6.7 page-faults/sec，说明内存完全足够。

---

**方法二：火焰图中的内存相关函数占比**

复用已有的 `perf record` 火焰图，通过**火焰图内置搜索**（不是浏览器的 Ctrl+F）来查找内存相关函数。

**操作步骤**：

```bash
# 1. 确保已生成火焰图（见第二章 2.5 节）
ls -lh flame.svg

# 2. 拷贝到宿主机共享目录
cp flame.svg /mnt/hical_host/
```

在 Windows 宿主机上用浏览器（Chrome / Edge / Firefox）打开 `flame.svg`。

> **注意**：不要用"双击打开"——某些 Windows 环境会用图片查看器打开 SVG，没有交互功能。
> 正确方式：右键 → 打开方式 → 选择浏览器，或直接拖拽到浏览器窗口。

火焰图在浏览器中的交互界面：

```
┌─────────────────────────────────────────────────────────────────┐
│  Hical bench_server (wrk 4t/100c/30s)          [Search] [ic]   │  ← 右上角
│                                                                  │
│  ┌── main ──────────────────────────────────────────────────┐   │
│  │  ┌── io_context::run ────────────────────────────────┐   │   │
│  │  │  ┌── handleSession ──────────┐ ┌── sendmsg ────┐ │   │   │
│  │  │  │  ┌── phr_parse ──┐       │ │               │ │   │   │
│  │  │  │  │               │       │ │               │ │   │   │
│  │  │  └──┴───────────────┴───────┘ └───────────────┘ │   │   │
│  │  └─────────────────────────────────────────────────┘   │   │
│  └────────────────────────────────────────────────────────┘   │
│                                                                  │
│  ← 鼠标悬停显示函数名和占比                                      │
│                                        matched: 0.0% of samples │  ← 搜索匹配结果
└─────────────────────────────────────────────────────────────────┘
```

**搜索操作**（3 种方式）：

| 方式     | 操作                         | 说明                                                      |
| -------- | ---------------------------- | --------------------------------------------------------- |
| 点击按钮 | 点击右上角 **"Search"** 按钮 | 弹出输入框                                                |
| 快捷键   | 按键盘 **`s`** 键            | 同上（注意不是 Ctrl+F）                                   |
| Ctrl+F   | **不要用**                   | 这是浏览器自带的文本搜索，搜的是 SVG 源码，不是火焰图函数 |

点击 Search 或按 `s` 后：

1. 页面顶部弹出输入框，输入关键词（支持**正则表达式**）
2. 按 Enter 确认
3. **匹配的函数块变为紫红色高亮**，不匹配的变暗
4. 右下角显示 **"matched: X.X% of samples"** — 这就是该函数占总 CPU 时间的比例
5. 按 **Esc** 或点击 "Reset Search" 清除搜索

**逐个搜索以下关键词**：

| 搜索关键词      | 含义                         | 判断                                          |
| --------------- | ---------------------------- | --------------------------------------------- |
| `memcpy`        | 大块内存拷贝                 | **> 5%** → 数据拷贝过多，考虑零拷贝/move 语义 |
| `memmove`       | 带重叠的内存拷贝             | **> 3%** → vector insert/erase 频繁           |
| `malloc`        | 堆内存分配                   | **> 5%** → 分配密集，考虑 PMR/预分配/栈缓冲   |
| `cfree\|free`   | 堆内存释放（正则匹配两个词） | **> 3%** → 与 malloc 配对，同上               |
| `operator new`  | C++ new 表达式               | 同 malloc                                     |
| `__memmove_avx` | AVX 优化的 memmove           | 出现即说明有大块拷贝                          |

**示例**：搜索 `malloc` 后右下角显示 "matched: 8.3% of samples"，说明 8.3% 的 CPU 时间花在了分配内存上——需要优化分配策略。

**搜索不生效的排查**：

| 现象               | 原因                               | 解决                                                                                                                  |
| ------------------ | ---------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| 点击 Search 没反应 | 浏览器阻止本地 SVG 执行 JavaScript | 用 `python3 -m http.server 9000` 在 VM 上起个 HTTP 服务，浏览器访问 `http://127.0.0.1:9000/flame.svg`（需要端口转发） |
| 搜索后没有高亮     | 搜索词在火焰图中不存在             | 正常，说明该函数占比极低或没出现                                                                                      |
| 用的 Ctrl+F        | 打开的是浏览器文本搜索             | 改用按 `s` 键或点右上角 Search 按钮                                                                                   |

---

**方法三：Heaptrack 的临时分配分析**

临时分配（分配后很快释放）和缓存的关系：每次 malloc 返回的新内存块大概率不在 L1/L2 缓存中，写入时触发 cache miss；释放后这块缓存行又作废了。高频临时分配 = 高频 cache pollution。

```bash
# ── 操作步骤 ──

# 1. 已有 heaptrack 数据文件（见 3.3 节）
FILE=heaptrack.bench_server.42147.zst

# 2. 查看临时分配排名
heaptrack_print $FILE | sed -n '/^MOST TEMPORARY/,/^total runtime/p' | head -60

# 3. 关注两个指标：
#    - 临时分配占总分配的百分比（< 1% 良好，> 10% 需优化）
#    - 临时分配的绝对频率（tail -10 看 "temporary memory allocations: XX/s"）
heaptrack_print $FILE | tail -10
```

**判断标准**：

| 指标         | 良好    | 需优化    | 优化方向                           |
| ------------ | ------- | --------- | ---------------------------------- |
| 临时分配占比 | < 1%    | > 10%     | 用 PMR / 栈分配 / reserve 替代     |
| 临时分配频率 | < 100/s | > 10000/s | 每秒万次临时分配 = 万次 cache miss |

> Hical 实测：41/s 临时分配，占比 0.24%——缓存友好。

---

**方法四：Cachegrind（Valgrind 工具，完全不依赖 PMU）**

Cachegrind 在软件层面**模拟** CPU 缓存行为（L1 data/instruction + LL），输出每个函数精确的 cache miss 数——不需要任何硬件支持。代价是 **20~50 倍减速**，只适合短程序或单元测试。

```bash
# ── 操作步骤 ──

# 1. 对单元测试运行 Cachegrind（短程序，几秒完成）
valgrind --tool=cachegrind \
    --branch-sim=yes \
    ./build/tests/test_router

# 输出文件：cachegrind.out.<pid>（如 cachegrind.out.43210）

# 2. 查看汇总结果
cg_annotate cachegrind.out.* | head -40

# 3. 只看特定源文件的逐行 cache miss
cg_annotate cachegrind.out.* --auto=yes \
    --include=src/core/Router.h | head -100

# 4. 对 bench_server 短时间运行（不推荐，太慢）
# 如果一定要跑服务器，限制连接数和时长：
valgrind --tool=cachegrind ./build/bench_server &
# 另一终端：wrk -t1 -c10 -d5s http://127.0.0.1:8080/
# 5 秒后 Ctrl+C 停止 bench_server
```

**Cachegrind 输出示例**：

```
==43210== I   refs:      125,432,100
==43210== I1  misses:        234,567
==43210== LLi misses:         12,345
==43210== I1  miss rate:        0.19%
==43210== LLi miss rate:        0.01%
==43210==
==43210== D   refs:       56,789,000  (38,901,000 rd + 17,888,000 wr)
==43210== D1  misses:      1,234,567  (   890,123 rd +    344,444 wr)
==43210== LLd misses:        123,456  (    89,012 rd +     34,444 wr)
==43210== D1  miss rate:        2.2%  (      2.3%    +       1.9%)
==43210== LLd miss rate:        0.2%  (      0.2%    +       0.2%)
```

**解读**：

| 指标            | 含义                     | 健康值                                 |
| --------------- | ------------------------ | -------------------------------------- |
| `D1 miss rate`  | L1 数据缓存未命中率      | < 5%                                   |
| `LLd miss rate` | 最后一级数据缓存未命中率 | < 1%                                   |
| `I1 miss rate`  | L1 指令缓存未命中率      | < 1%（高 = 代码膨胀/icache thrashing） |

**`cg_annotate` 逐行输出**：

```bash
cg_annotate cachegrind.out.* --auto=yes --include=src/core/Router.h
```

输出每行代码的 cache miss 数：

```
-- line 156 ----------------------------------------
         Dr        D1mr       DLmr
        567         45          2   :   auto it = m_staticRoutes.find(key);
         89          0          0   :   if (it != m_staticRoutes.end()) {
        234         12          1   :       return it->second;
```

- `Dr` = 数据读取次数
- `D1mr` = L1 数据缓存读取未命中
- `DLmr` = LL 数据缓存读取未命中（最严重，需从内存取）

哪一行 `D1mr` 高，哪一行就是缓存不友好的热点。

---

**四种方法的选择建议**：

| 场景                           | 推荐方法                        | 原因               |
| ------------------------------ | ------------------------------- | ------------------ |
| 快速排除内存压力问题           | 方法一（perf stat page-faults） | 1 条命令，秒出结果 |
| 已有火焰图，顺便看缓存         | 方法二（搜索 malloc/memcpy）    | 零额外成本         |
| 分析分配模式对缓存的影响       | 方法三（Heaptrack 临时分配）    | 已有数据直接看     |
| 需要精确的逐行 cache miss 数据 | 方法四（Cachegrind）            | 最精确，但最慢     |

### 4.5 影响 Hical 性能的缓存效应

#### 效应一：HeaderMap 的线性扫描

`src/core/HeaderMap.h` 使用 `vector<pair<string,string>>` 存储 HTTP 头部，线性扫描查找。

为什么这**反而比 unordered_map 快**：
- 典型 HTTP 请求只有 10~20 个头部
- `vector` 内存连续 → 一次缓存预取可以加载多个头部
- `unordered_map` 每个桶是独立的堆分配 → 指针追踪，每个桶可能触发 cache miss
- 20 个 `pair<string_view, string_view>` ≈ 640 字节 ≈ 10 个 cache line → 完全在 L1 中

```
vector<pair>: [hdr0][hdr1][hdr2][hdr3]...   ← 连续内存，CPU 预取友好
               ↓一次加载 64 字节↓

unordered_map: bucket[0] → node → pair       ← 每次 node 访问可能 cache miss
               bucket[1] → nullptr
               bucket[2] → node → pair → node → pair
```

#### 效应二：Router 的 TransparentHash

`src/core/Router.h` 中的静态路由使用 `RouteKeyView` + `is_transparent` 实现零拷贝 `string_view` 查找。

缓存优势：
- 查找时不需要构造 `std::string` → 避免堆分配（避免 cache pollution）
- hash 计算直接在 `string_view` 指向的原始缓冲区上进行 → 数据已在缓存中

#### 效应三：零拷贝 HTTP 解析

`HttpRequest::fromParsed()` 存储 `string_view` 引用连接级 `readBuf`，不拷贝头部数据。

缓存优势：
- `readBuf` 是一块连续内存，picohttpparser 解析时已把它加载到缓存
- 后续 `header()`/`path()` 访问的是同一块内存 → L1 命中
- 如果改成拷贝，每个请求多 1~2KB 堆分配 → L1 被冲刷

#### 效应四：False Sharing（伪共享）

当两个线程频繁写入同一 cache line 上的不同变量时，会触发 **cache line bouncing**——两个核心的缓存控制器互相 invalidate 对方的缓存行，即使它们逻辑上互不相关。

```cpp
// 危险模式：两个高频写入的变量可能在同一 cache line 上
struct SharedStats
{
    std::atomic<uint64_t> m_threadACounter;  // 线程 A 高频写
    std::atomic<uint64_t> m_threadBCounter;  // 线程 B 高频写
    // 如果这两个变量在同一个 64 字节 cache line 内 → false sharing
};

// 修复：对齐到 cache line 边界
struct alignas(64) PaddedCounter
{
    std::atomic<uint64_t> value;
};

struct SharedStats
{
    PaddedCounter m_threadACounter;  // 独占 cache line
    PaddedCounter m_threadBCounter;  // 独占 cache line
};
```

检测 false sharing：

```bash
# perf c2c（cache-to-cache）分析——检测 cache line 争用
# 注意：VirtualBox 下可能不可用，需裸机或 KVM
sudo perf c2c record -p $SERVER_PID -- sleep 30
sudo perf c2c report --stdio
```

### 4.6 缓存优化原则速查

| 原则                   | 做法                                      | Hical 中的实践                         |
| ---------------------- | ----------------------------------------- | -------------------------------------- |
| **数据局部性**         | 连续访问的数据放在连续内存中              | HeaderMap 用 vector 而非 map           |
| **紧凑数据结构**       | 减少结构体大小，让更多元素装入 cache line | NativeRequest 用 stack array[64]       |
| **避免指针追踪**       | 用数组/vector 替代链表/树                 | 参数路由用 `vector<ParamRoute>`        |
| **减少堆分配**         | 用栈分配/PMR/预分配替代 new               | FixedBuffer<4096>、PMR 三级池          |
| **避免 false sharing** | 高频写的跨线程变量对齐到 64 字节          | `lastActiveTimeMs_` 是 atomic 独立变量 |
| **热冷分离**           | 高频访问字段放在结构体前部                | NativeRequest 把 method/path 放前面    |

---

## 五、实战：Hical 性能分析全流程

将前面所有工具串联，完成一次完整的性能分析。

### 5.1 准备

```bash
cd ~/projects/Hical

# 编译 RelWithDebInfo（带符号的优化版本）
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=gcc-14 \
    -DCMAKE_CXX_COMPILER=g++-14 \
    -DHICAL_BUILD_BENCH=ON
cmake --build build -j$(nproc)

# 确保 perf 权限
sudo sysctl -w kernel.perf_event_paranoid=-1
sudo sysctl -w kernel.kptr_restrict=0

# 启动 bench_server
./build/bench_server &
SERVER_PID=$!
curl -s http://127.0.0.1:8080/ && echo " OK"
```

### 5.2 第一步：perf stat 初步判断

```bash
# VirtualBox 下用软件事件采集 30 秒（同时施压）
perf stat -e task-clock,cpu-clock,context-switches,cpu-migrations,\
page-faults,minor-faults,major-faults \
    -p $SERVER_PID -- sleep 30 &
STAT_PID=$!

wrk -t4 -c100 -d30s http://127.0.0.1:8080/
wait $STAT_PID
```

**判断**（VirtualBox 软件事件）：
- CPUs utilized 接近核心数 → CPU 密集，继续火焰图找热点函数
- CPUs utilized < 1.0 → I/O-bound 或锁等待，用 strace 看阻塞点
- context-switches > 10000/sec → 可能有锁竞争
- major-faults > 0（稳态）→ 内存不足

> **裸机/KVM 额外判断**：IPC > 1.0 + cache-misses < 5% → CPU-bound；IPC < 0.5 + cache-misses > 10% → Memory-bound

### 5.3 第二步：perf record + 火焰图定位 CPU 热点

```bash
sudo perf record -g -F 999 -p $SERVER_PID -o perf.data -- sleep 30 &
PERF_PID=$!

wrk -t4 -c100 -d30s http://127.0.0.1:8080/
wait $PERF_PID

# 生成火焰图
sudo perf script -i perf.data > perf_out.txt
~/FlameGraph/stackcollapse-perf.pl perf_out.txt > perf.folded
~/FlameGraph/flamegraph.pl --title "Hical Hello World" perf.folded > flame.svg

cp flame.svg /mnt/hical_host/
```

**分析火焰图**：
- 如果用户态（Hical 框架代码）占比 < 5%，说明框架效率高，瓶颈在内核/Asio
- 如果某个用户态函数占比 > 5%，进入 `perf annotate` 看逐行热点

### 5.4 第三步：Heaptrack 分析内存分配

需要**两个终端**（与 perf record 不同，heaptrack 启动时录制会占住终端）：

```bash
# ── 终端 1 ──
pkill bench_server
heaptrack ./build/bench_server
# 终端被占住，等终端 2 压测完后 Ctrl+C
```

```bash
# ── 终端 2 ──
wrk -t4 -c100 -d30s http://127.0.0.1:8080/
# 压测结束后回终端 1 按 Ctrl+C
```

```bash
# ── 终端 1（Ctrl+C 后继续） ──

# 分析（文件名以实际输出的为准）
heaptrack_print heaptrack.bench_server.*.zst | head -150

# 或生成内存分配火焰图
heaptrack_print --flamegraph-cost-type allocations \
    heaptrack.bench_server.*.zst > heap.folded
~/FlameGraph/flamegraph.pl --title "Heap Allocations" \
    --countname "allocs" --colors mem \
    heap.folded > heap_flame.svg

cp heap_flame.svg /mnt/hical_host/
```

**关注**：
- 临时分配 > 10 万次？→ 考虑用 PMR 或栈分配替代
- 峰值内存远超稳态？→ 可能有容器未 reserve 导致反复扩容

### 5.5 第四步：综合分析与优化决策

| 发现                           | 优化方向                            | 工具验证            |
| ------------------------------ | ----------------------------------- | ------------------- |
| IPC 低 + cache-misses 高       | 数据布局优化（向量化、紧凑结构体）  | perf stat 再次对比  |
| 火焰图某函数 > 5%              | 算法优化或内联                      | 差异火焰图 A/B 对比 |
| heaptrack 临时分配多           | PMR / 栈分配 / reserve              | heaptrack 再次对比  |
| `malloc/free` 在火焰图上占比高 | 替换分配器（jemalloc/tcmalloc/PMR） | heaptrack + 火焰图  |

### 5.6 清理

```bash
pkill bench_server
rm -f perf.data perf_out.txt perf.folded
# heaptrack 数据文件按需保留
```

---

## 六、速查卡

### perf stat 速查

```bash
# 默认软件事件（VirtualBox 可用）
perf stat ./build/tests/test_router

# 指定软件事件（VirtualBox 可用）
perf stat -e task-clock,cpu-clock,context-switches,cpu-migrations,\
page-faults,minor-faults,major-faults -p $PID -- sleep 30

# 指定硬件事件（需裸机/KVM）
perf stat -e cycles,instructions,cache-misses -p $PID -- sleep 30

# 多次运行取平均
perf stat -r 5 ./build/tests/test_router

# 分组避免多路复用（需裸机/KVM）
perf stat -e '{cycles,instructions}' -e '{cache-references,cache-misses}' ...
```

### perf record + 火焰图速查

```bash
# 录制（附着进程，999Hz，30秒）
sudo perf record -g -F 999 -p $PID -o perf.data -- sleep 30

# 快速查看
sudo perf report -i perf.data

# 生成火焰图（三步管道）
sudo perf script -i perf.data > out.txt
~/FlameGraph/stackcollapse-perf.pl out.txt > out.folded
~/FlameGraph/flamegraph.pl out.folded > flame.svg

# 逐行热点
sudo perf annotate -i perf.data -s <函数名>
```

### Heaptrack 速查

```bash
# 启动时录制（推荐，需要两个终端）
# 终端 1: heaptrack ./build/bench_server    ← 占住终端
# 终端 2: wrk -t4 -c100 -d30s http://127.0.0.1:8080/
# 压测完回终端 1 按 Ctrl+C

# 附着进程（需要 ptrace_scope=0，不如方式一可靠）
heaptrack --pid $PID

# 文本分析
heaptrack_print heaptrack.*.zst

# GUI 分析
heaptrack_gui heaptrack.*.zst

# 生成分配火焰图
heaptrack_print --flamegraph-cost-type allocations heaptrack.*.zst > heap.folded
~/FlameGraph/flamegraph.pl --countname allocs --colors mem heap.folded > heap.svg
```

### 缓存分析速查

```bash
# 查看缓存拓扑
lscpu | grep -i cache

# 量化 cache miss（需裸机/KVM）
perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses ...

# VirtualBox 替代：Cachegrind 模拟（极慢，适合短程序）
valgrind --tool=cachegrind ./build/tests/test_router
cg_annotate cachegrind.out.*

# VirtualBox 替代：火焰图搜索 malloc/memcpy 间接判断
# 在浏览器 SVG 搜索框输入 "malloc" 或 "memcpy"

# false sharing 检测（需裸机/KVM）
sudo perf c2c record -p $PID -- sleep 30
sudo perf c2c report --stdio
```

### 判断瓶颈类型速查

**VirtualBox（软件事件）**：

| 指标             | CPU 密集       | I/O / 锁等待         | 内存分配密集        |
| ---------------- | -------------- | -------------------- | ------------------- |
| CPUs utilized    | 接近核心数     | < 1.0                | 高                  |
| context-switches | < 1000/sec     | > 10000/sec          | 正常                |
| 火焰图热点       | 用户态业务函数 | `epoll_wait`/`futex` | `malloc`/`memcpy`   |
| 优化方向         | 算法/指令数    | 异步I/O/减少阻塞     | PMR/预分配/数据布局 |

**裸机/KVM（硬件事件）**：

| 指标          | CPU-bound   | Memory-bound      | I/O-bound            |
| ------------- | ----------- | ----------------- | -------------------- |
| IPC           | > 1.0       | < 0.5             | 无关                 |
| CPUs utilized | 高          | 高                | < 1.0                |
| cache-misses  | < 5%        | > 10%             | 无关                 |
| 火焰图热点    | 用户态函数  | `malloc`/`memcpy` | `epoll_wait`/`futex` |
| 优化方向      | 算法/指令数 | 数据布局/分配器   | 异步I/O/减少阻塞     |
