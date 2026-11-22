<div align="center">
  <img src="./Logo.png" alt="TsnHub Logo" width="120" />
  <h2>TsnHub</h2>
  <h3>基于 open62541 的 Linux 本地 TSN 仿真中间节点</h3>
</div>

## 一、项目简介

TsnHub 是 FasterEdge 生态中的 TSN 仿真组件。它在 OPC UA PubSub 数据的接收与重新发布之间加入用户态 TSN 调度器，用于在普通 Linux 主机或 Docker 环境中模拟时间敏感网络的队列、优先级、门控、时延、抖动、丢包与拥塞行为。

上述每一项都可以被测量：时延与抖动按优先级输出 p50/p99/max（见「九、时延与拥塞测量」），拥塞由 `--shape-mbps` 令牌桶与 `--credit-frames-per-window` 窗口配额两级限速实现。注意两个丢包口径的区别：`loss_rate = dropped_loss / offered` 只反映 `--loss` 指定的随机丢包（`tsn/Scheduler.cpp:69`，与负载无关），随负载增长的是 `drop_rate = (dropped_loss + dropped_queue) / offered`，因为 `dropped_queue` 来自队列容量溢出；画丢包-负载曲线应取 `offered` 为横轴、`drop_rate` 为纵轴。

目标数据流：

```text
OPC UA Publisher
       |
       | UDP/UADP
       v
open62541 DataSetReader
       |
       v
User-space TSN Scheduler
  - 8 priority queues
  - Gate Control List
  - delay / jitter / loss
  - queue capacity
       |
       v
open62541 DataSetWriter
       |
       | UDP/UADP
       v
OPC UA Subscriber
```

当前仓库已经实现 Linux 原生 open62541 UDP/UADP DataSetReader/DataSetWriter、用户态 TSN 调度器、Portable PubSub 兼容模式，以及 Publisher → TsnHub → Subscriber 三节点 Docker 仿真环境。

## 二、术语

- **Frame**：进入 TsnHub 的一条待调度消息，包含 sequence、priority、stream 与 payload。
- **Priority Queue**：优先级 0～7 的八个独立队列，7 为最高优先级。
- **Gate Control List（GCL）**：按周期控制哪些优先级队列可以发送的时隙列表。
- **Portable PubSub**：跨平台 UDP 封装，payload 可透明承载 OPC UA PubSub/UADP 网络消息。
- **Native PubSub**：Linux 下由 open62541 原生 DataSetReader/DataSetWriter 完成的 UDP/UADP 收发。
- **SimulatorNode**：连接入口、调度器和出口的中间节点。

## 三、核心能力

| 能力 | 说明 |
|---|---|
| 八级优先级 | 支持优先级 0～7，并优先释放高优先级帧 |
| Gate Control List | 使用重复时隙定义不同优先级队列的开放窗口 |
| 绝对时刻门控唤醒 | 按 GCL 边界的绝对时刻唤醒（Linux `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`，Windows 高精度 waitable timer），门控相位与轮询粒度解耦 |
| Paced 出队（信用） | 单个 GCL 开放窗口内下发帧数受 `--credit-frames-per-window` 上限约束，超出部分顺延到下一个同类窗口，不会一次抽空整队 |
| 出口整形（令牌桶） | 按 `--shape-mbps` 端口线速把帧序列化成字节流，令牌不足时推迟而非丢弃 |
| 固定时延 | 为所有通过节点的帧增加固定转发延迟 |
| 随机抖动 | 在固定时延基础上增加可配置的对称随机抖动 |
| 丢包仿真 | 按 0～1 的概率随机丢弃输入帧 |
| 队列容量 | 限制每个优先级队列的最大积压帧数 |
| 可复现实验 | 通过固定随机种子复现抖动和丢包结果 |
| 运行统计 | 输出 accepted、released、dropped_loss、dropped_queue、shaped_deferrals、credit_deferrals |
| 端到端时延统计 | 按优先级统计 p50/p90/p99/max/mean 时延、抖动与乱序，CSV/JSON 可导出 |
| UDP 转发 | 支持 Linux/macOS BSD Socket 与 Windows Winsock2 |
| open62541 | 链接 open62541 1.5.9；Linux 原生 PubSub 模式使用 UDP/UADP |

## 四、目录结构

```text
TsnHub/
├── main.cpp                     # CLI 与进程入口
├── tsn/
│   ├── Frame.hpp                # 调度帧定义
│   ├── GateClock.hpp/.cpp       # GCL 相位计算与绝对时刻睡眠（跨平台）
│   ├── LatencyStats.hpp/.cpp    # 端到端时延/抖动/乱序统计（确定性时间源）
│   ├── PriorityMap.hpp/.cpp     # PublisherId/WriterGroupId/DataSetWriterId -> 优先级映射
│   ├── Scheduler.hpp/.cpp       # 用户态 TSN 调度器
│   ├── StatsReport.hpp/.cpp     # 统计输出（摘要行/分位数表/CSV/JSON）
│   └── SimulatorNode.hpp/.cpp   # UDP 中间节点
├── transport/
│   ├── UdpSocket.hpp/.cpp       # 跨平台 UDP 封装
│   └── PortablePubSub.hpp/.cpp  # Portable PubSub Envelope
├── tests/
│   ├── SchedulerTest.cpp
│   ├── SchedulerPacingTest.cpp   # 单窗口帧数上限、信用复位、门控相位误差
│   ├── SimulatorNodePacingTest.cpp # 端到端窗口 pacing（真实 UDP 回环）
│   ├── SchedulerLatencyTest.cpp  # 令牌桶整形记账、分位数/CSV/JSON 输出、丢包计数器
│   ├── GateClockTest.cpp        # 时隙相位表、边界时刻、绝对睡眠精度
│   ├── LatencyStatsTest.cpp     # 时延/抖动/乱序统计（注入确定性时间源）
│   ├── PortablePubSubTest.cpp
│   ├── PriorityMapTest.cpp
│   ├── UdpLoopbackTest.cpp
│   ├── SimulatorNodeTest.cpp
│   └── TestAssertions.hpp         # NDEBUG 无关的测试断言宏
├── CMakeLists.txt
├── conanfile.py
├── conandata.yml
├── conan_provider.cmake        # CMake 的 Conan 提供者桥接（配合 conan install --build=missing）
├── SECURITY.md                 # 安全策略与漏洞报告渠道
└── Logo.png
```

原生 PubSub 与 Docker 文件：

```text
├── pubsub/Open62541Bridge.*     # open62541 DataSetReader/DataSetWriter
├── docker/PubSubFixture.cpp     # Publisher / Subscriber 测试程序
├── Dockerfile                   # open62541 源码构建与运行镜像
├── docker-compose.yml           # 三节点仿真拓扑
└── tests/native_pubsub_integration.sh
```

## 五、构建依赖

- Linux（原生 PubSub 最终运行环境）
- CMake >= 3.23
- Conan 2.x
- C++17 编译器
- CLI11 2.6
- open62541 1.5.9

Portable PubSub 模式也可以在 macOS 和 Windows 构建。open62541 原生 UDP/UADP PubSub 模式统一以 Linux/Docker 为目标环境。

Windows 上跑单元测试有一个前提：Git for Windows 自带的 `mingw64/bin`（Git Bash 里是 `/mingw64/bin`，
实际路径 `D:/Environment/Git/mingw64/bin`）里有一份 ABI 不同的 `libstdc++-6.dll`，它默认排在
`PATH` 前面；若它先于 WinLibs 的运行时被加载，测试进程会直接段错误。因此必须把 WinLibs 的
`mingw64/bin` 目录**前置**到 `PATH`，例如：

```bash
export PATH="/c/Users/Dlut/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"
```

同一目录下的 MinGW 工具链也是构建本身所需（`cmake -G "MinGW Makefiles"`）。

## 六、本地构建

```bash
conan install . \
  --output-folder=build-tsn \
  --build=missing \
  -s build_type=Debug

cmake -S . -B build-tsn/cmake \
  -DCMAKE_TOOLCHAIN_FILE=build-tsn/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build-tsn/cmake -j
ctest --test-dir build-tsn/cmake --output-on-failure
```

没有 conan / open62541 / CLI11 时，Portable 模式可独立配置与测试：

```bash
cmake -S . -B build-portable -DTSNHUB_USE_OPEN62541_PUBSUB=OFF
cmake --build build-portable -j
ctest --test-dir build-portable --output-on-failure
```

此时 CLI 可执行文件 `TsnHub` 与原生 PubSub 目标会被跳过，配置阶段会打印
`CLI11 not found` / `open62541 not found` 说明原因；`tsnhub_core` 与各单元测试
不依赖这两个包，始终可构建。若显式要求 `TSNHUB_USE_OPEN62541_PUBSUB=ON` 却找不到
open62541，配置阶段直接报错，不会静默降级成 Portable 模式。

**没有 open62541（或没有 CLI11）时不存在 `TsnHub` 可执行文件，因此 §九 里以
`--stats-format` / `--stats-file` / `--run-ms` 驱动的测量命令也跑不了。** 原因是硬性的：
`main.cpp:8` 无条件 `#include <open62541/config.h>`（`--capabilities` 的版本号输出、
原生模式的 URL 解析都要它），`CMakeLists.txt:66` 的 `if(CLI11_FOUND AND open62541_FOUND)`
要求两者同时存在才创建 `TsnHub` 目标。此条件下能验证的只有 `tsnhub_core` 与 10 个
单元测试（见 §十三）；要产出 CSV/JSON 统计数据集，必须先装好 open62541 与 CLI11，
再按 §六 的 conan 流程或 §十二 的 Docker 镜像构建。

查看当前构建能力：

```bash
./build-tsn/cmake/TsnHub --capabilities
```

输出示例：

```text
open62541=1.5.9
native_pubsub=false
portable_pubsub=true
portable_scheduler=true
latency_stats=true
egress_shaping=true
stats_formats=summary,table,csv,json
```

在 Linux 原生 PubSub 构建中，`native_pubsub` 应为 `true`。

## 七、Portable PubSub 运行方式

```bash
./build-tsn/cmake/TsnHub \
  --listen 127.0.0.1:4841 \
  --forward 127.0.0.1:4842 \
  --gate 1000:0x0f \
  --gate 1000:0xf0 \
  --delay-us 200 \
  --jitter-us 50 \
  --loss 0.01 \
  --queue 1024 \
  --credit-frames-per-window 16 \
  --shape-mbps 100 \
  --seed 42
```

参数说明：

| 参数 | 说明 |
|---|---|
| `--listen host:port` | 输入 UDP 地址 |
| `--forward host:port` | 输出 UDP 地址 |
| `--gate duration_us:mask` | Gate 时隙，可重复指定；mask 的 bit 0～7 对应优先级 0～7 |
| `--delay-us` | 固定转发时延，单位微秒 |
| `--jitter-us` | 随机抖动范围，实际值为 ±jitter |
| `--loss` | 0～1 的随机丢包概率 |
| `--queue` | 每个优先级最大排队帧数 |
| `--shape-mbps` | 出口令牌桶整形速率（Mbit/s），0 表示关闭 |
| `--shape-burst-bytes` | 令牌桶深度（字节），0 表示 64 KiB 默认值 |
| `--credit-frames-per-window` | 单个开放 GCL 窗口内最多下发的帧数，0 表示关闭（关闭时沿用"到期帧全部放行"的旧行为） |
| `--stats-format` | 统计输出格式：`summary`（默认）、`table`、`csv`、`json` |
| `--stats-file` | 把统计写到指定文件；不指定时全部打到 stdout |
| `--run-ms` | 运行固定毫秒数后自行停止并输出统计；0 表示一直运行到 SIGINT/SIGTERM |
| `--seed` | 随机种子 |
| `--capabilities` | 输出构建能力并退出 |

原生模式（`--mode native`，需要 open62541 + CLI11）专用参数：

| 参数 | 说明 |
|---|---|
| `--mode` | 传输模式：`portable`（默认）或 `native` |
| `--subscribe-url` | 原生模式输入 URL（`opc.udp://0.0.0.0:4841`） |
| `--publish-url` | 原生模式输出 URL |
| `--publisher-bind-url` | 原生模式 writer 的**本地绑定** URL；open62541 ≥ 1.5 拒绝 UDP 端口 0，因此它必须是固定端口（默认 `opc.udp://0.0.0.0:4843`），且不能与 `--subscribe-url` 或同一主机上 subscriber 的绑定端口冲突 |
| `--input-publisher-id` | 期望的上游 PublisherId |
| `--input-writer-group-id` | 期望的上游 WriterGroupId |
| `--input-writer-id` | 期望的上游 DataSetWriterId |
| `--output-publisher-id` | TsnHub 输出的 PublisherId |
| `--output-writer-group-id` | TsnHub 输出的 WriterGroupId |
| `--output-writer-id` | TsnHub 输出的 DataSetWriterId |
| `--publish-interval-ms` | 原生模式发布间隔（毫秒） |
| `--priority-rule` | 原生模式优先级规则 `publisher:writerGroup:writer:priority`，可重复；单个分量可写 `*` 通配，但三个分量全通配会被拒绝 |
| `--priority-map-file` | 每行一条优先级规则的纯文本文件，支持 `#` 注释与空行 |
| `--input-default-priority` | 没有规则命中时使用的优先级（默认 0，取值 0～7） |

按 `Ctrl+C` 或发送 `SIGTERM` 后，节点停止并输出统计信息。

## 八、Gate 配置示例

```bash
--gate 1000:0x0f --gate 1000:0xf0
```

表示一个 2000 微秒周期：

1. 前 1000 微秒开放优先级 0～3；
2. 后 1000 微秒开放优先级 4～7。

如果不提供 `--gate`，默认每 1000 微秒开放全部优先级：

```text
1000:0xff
```

## 九、时延与拥塞测量

`Frame.receivedAt` 在入队时打上时间戳，出队时与当前时刻相减得到**节点内驻留时延**，
`tsn/LatencyStats.cpp` 按优先级累计成 p50/p90/p99/max/mean 与抖动，并统计同一 stream 内
sequence 回退（乱序）与重复的次数。

时间源由调用方以 `std::chrono::steady_clock::time_point` 显式传入，统计器本身不读真实时钟；
同一组输入必然得到同一组输出，因此单元测试可以用注入的确定性时间逐位断言分位数。

分位数采用最近秩法（nearest-rank）：第 q 分位数是排序后第 `ceil(q*n)` 小的样本（1 起），
`n=10` 时 p50 是第 5 小、p99 是第 10 小（即 max）。样本量超过每优先级保留上限
（`SchedulerConfig::latencyRetention`，默认 16384）后启用固定种子的蓄水池抽样，
内存占用有界；此时 `retained` 字段给出实际参与分位计算的样本数，`samples` 始终是全部帧数。

运行结束时按 `--stats-format` 输出（这些开关属于 `TsnHub` CLI，需要 open62541 + CLI11 才能构建，
见 §六）：

| 格式 | 内容 |
|---|---|
| `summary` | 单行计数器（默认），保持既有 `accepted=/released=/dropped_loss=/dropped_queue=` 前缀，另附 `shaped_deferrals=`、`credit_deferrals=`、`latency_samples=` |
| `table` | 人类可读的按优先级分位数表 |
| `csv` | 每优先级一行，含 min/p50/p90/p99/max/mean/jitter、乱序、重复、offered 与 loss_rate/drop_rate |
| `json` | 与 CSV 同源的 JSON 对象：`totals` 含 accepted/released/dropped_loss/dropped_queue/shaped_deferrals/credit_deferrals/latency_samples，`priorities` 数组含 CSV 的全部逐优先级字段，`note` 字段回带本次运行的配置 |

CSV 的 `offered = accepted + dropped_loss + dropped_queue`，与 `loss_rate`、`drop_rate`
一起构成丢包-负载曲线的最小充分统计量。注意两列口径不同：
`loss_rate = dropped_loss / offered` 只统计 `--loss` 指定的随机丢包，其期望值就是 `--loss`
本身，与负载无关；`drop_rate = (dropped_loss + dropped_queue) / offered` 才包含队列容量
溢出（`dropped_queue`），会随负载上升。因此画"随负载变化的丢包曲线"应以 `offered` 为横轴、
`drop_rate` 为纵轴；若横轴用 `loss_rate`，得到的是一条水平线（等于 `--loss`），不能反映拥塞。

`--stats-file` 可把统计重定向到文件，方便一次实验写一份数据集；不指定时全部打到 stdout。

`--run-ms` 让节点在固定窗口后自行停止并落盘统计：Windows 下 MSYS 的 `kill` 只做
`TerminateProcess`，进程拿不到信号、统计输出就永远打不出来，脚本化实验因此拿不到数据；
给出窗口长度后，无论有没有信号通道，一次运行都会产出完整统计。

```bash
./build-release/TsnHub \
  --listen 127.0.0.1:4841 --forward 127.0.0.1:4842 \
  --shape-mbps 100 --delay-us 200 --loss 0.01 \
  --stats-format csv --stats-file run-100m.csv --run-ms 60000
```

拥塞由两级限速实现：`--shape-mbps` 令牌桶把出队序列化成端口线速上的字节流，令牌不足则推迟
并计入 `shaped_deferrals`；`--credit-frames-per-window` 再限制单个 GCL 窗口的下发帧数，超出部分
顺延到下一个窗口并计入 `credit_deferrals`。两者都只推迟、不丢弃，推迟帧仍占队列容量。

## 十、Portable PubSub Envelope

```text
magic      4 bytes  "TSN1"
priority   1 byte   0..7
sequence   8 bytes  big-endian
streamLen  2 bytes  big-endian
stream     N bytes  UTF-8
payload    remaining bytes
```

payload 对 TsnHub 保持透明，可以放入 open62541 或其他 OPC UA 实现生成的 UADP 网络消息。

## 十一、Linux 原生 open62541 PubSub

Linux 原生模式采用 open62541 的以下组件：

- `UA_PubSubConnectionConfig`
- `UA_ReaderGroupConfig`
- `UA_DataSetReaderConfig`
- `UA_Server_DataSetReader_createTargetVariables`
- `UA_PublishedDataSetConfig`
- `UA_DataSetFieldConfig`
- `UA_WriterGroupConfig`
- `UA_DataSetWriterConfig`

预期处理链路：

```text
DataSetReader subscribed variable
              |
              v
         TSN Scheduler
              |
              v
DataSetWriter published variable
```

第一版原生 DataSet 使用 `Int32` 字段，便于验证 Publisher → TsnHub → Subscriber 的完整链路。

原生模式运行示例：

```bash
TsnHub --mode native \
  --subscribe-url opc.udp://0.0.0.0:4841 \
  --publisher-bind-url opc.udp://0.0.0.0:4843 \
  --publish-url opc.udp://subscriber:4842 \
  --input-publisher-id 1 \
  --input-writer-group-id 1 \
  --input-writer-id 1 \
  --output-publisher-id 2 \
  --output-writer-group-id 2 \
  --output-writer-id 2 \
  --publish-interval-ms 1 \
  --delay-us 200
```

`--publisher-bind-url` 不能省略：open62541 把 PubSubConnection URL 当作 writer 的本地绑定地址
（`pubsub/Open62541Bridge.cpp:161`），而 open62541 ≥ 1.5 拒绝 UDP 端口 0。省略该参数时 writer
会落到默认的临时端口，在某些版本上直接导致连接创建失败；把它设为与 `--subscribe-url` 不同的
固定端口即可复现三节点链路。

### 原生模式优先级映射

原生模式下每一条 inbound 流的优先级由 `(PublisherId, WriterGroupId, DataSetWriterId)`
三元组决定，规则通过 `--priority-rule`（可重复）或 `--priority-map-file`（每行一条）给出。
`--priority-map-file` 指向的纯文本文件支持 `#` 起始的行注释与空行（`PriorityMap::loadFile`
会先剥掉 `#` 及其后内容再逐行解析），因此可以按 stream 分组并加说明：

```bash
TsnHub --mode native \
  --input-publisher-id 1 --input-writer-group-id 1 --input-writer-id 1 \
  --priority-rule 1:1:1:6 \
  --gate 1000:0x40
```

等价的映射文件写法（`#` 之后为注释，装载时被剥掉）：

```text
# 1:1:1:6
1:1:1:6
```

规则格式为 `publisher:writerGroup:writer:priority`，`priority` 取 0～7（7 最高）。
三个分量中的任意一个可写成 `*` 表示通配；匹配时取**非通配分量最多**的一条规则，
同具体度且可能同时命中的两条规则会在装载期报错退出，保证解析结果唯一。
`*:*:*:p` 这类三个分量全通配的规则会被拒绝（`PriorityMap::add` 要求至少一个具体分量），
需要"全部流量同一个优先级"时应改用 `--input-default-priority`。
未命中任何规则时同样使用 `--input-default-priority`（默认 0）。

之所以需要这层映射：open62541 的 DataSetReader 只接受与配置三元组完全一致的网络
消息，因此该三元组就是帧的身份；而早期实现把 `frame.priority` 硬编码为 0，所有帧
都挤进 0 号队列，`--gate` 的位掩码只有 bit0 生效，八级队列与 GCL 在原生路径上
完全退化。下面的对照实验可在原生构建上复现：

| GCL | 优先级规则 | Subscriber 结果 |
|---|---|---|
| `1000:0xff` | 无 | `subscriber final_value=20` |
| `1000:0x40` | `--priority-rule 1:1:1:6` | `subscriber final_value=20` |
| `1000:0x40` | 无（默认优先级 0） | `subscriber final_value=0` |

第三行即退化本身：GCL 只开放优先级 6，而帧仍被硬编码在 0 号队列，因此一帧都放不出去。

### 门控唤醒与 paced 出队

旧实现用 `sleep_for(1ms)` 轮询驱动门控，时隙短于 1ms 时相位由轮询粒度决定而不是时钟；
`releaseReady` 又在一个窗口内把整队到期帧一次抽空，属于突发下发而非 TAS 的 paced 语义。
现在两条路径分别改为：

1. **绝对时刻唤醒**：`tsn/GateClock.cpp` 里的 `sleepUntilMonotonic` 在 Linux 用
   `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`，Windows 用
   `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`，
   由宏 `_WIN32` 隔离。`SimulatorNode::run` 每次睡到当前 GCL 窗口结束的**绝对边界**，
   收包等待时间也被裁剪到不超过该边界，因此窗口相位不随调度延迟累积漂移。
2. **paced 出队（信用）**：`SchedulerConfig::creditFramesPerWindow` 限制单个开放窗口
   的下发帧数，超出部分留在队列里顺延到下一个窗口并计入 `credit_deferrals`。
   信用按窗口共享（高优先级不能挤占低优先级的配额），关闭时隙默认不消耗配额。
   与 `--shape-mbps` 的令牌桶形成"窗口配额 + 端口线速"两级约束，任一触发都只推迟、
   不丢弃。`creditFramesPerWindow = 0` 保留旧的"到期帧全部放行"行为。

对应测试：`gate_clock` 校验相位表与唤醒精度（不得早于 deadline、超时不超过
一个调度 tick），`scheduler_pacing` 校验单窗口帧数不超过上限、跨窗口配额复位、
关闭时隙不消耗配额、门控相位误差在阈值内。

## 十二、Docker 三节点仿真

Dockerfile 从源码构建 open62541 1.5.9，并开启：

```text
UA_ENABLE_PUBSUB=ON
UA_ENABLE_PUBSUB_INFORMATIONMODEL=ON
```

构建镜像：

```bash
docker build -t tsnhub .
```

启动 Publisher、TsnHub 和 Subscriber：

```bash
docker compose up --build --abort-on-container-exit
```

预期结果包括：

```text
publisher final_value=20
accepted=20 released=20 dropped_loss=0 dropped_queue=0
subscriber final_value=20
```

停止并清理：

```bash
docker compose down --remove-orphans
```

## 十三、测试

| 测试 | 覆盖内容 |
|---|---|
| `scheduler` | Gate、优先级、时延、丢包和队列容量 |
| `portable_pubsub` | Envelope 编解码与畸形包拒绝 |
| `udp_loopback` | 真实 UDP 回环收发与超时 |
| `simulator_node` | UDP 入口 → Scheduler → UDP 出口端到端转发 |
| `priority_map` | `PublisherId:WriterGroupId:WriterId:priority` 规则解析、通配、歧义拒绝，以及映射结果对 GCL 的实际驱动 |
| `gate_clock` | GCL 时隙相位表、窗口边界绝对时刻、绝对时刻睡眠的唤醒精度 |
| `scheduler_pacing` | 单窗口下发帧数不超过信用上限、跨窗口配额复位、关闭时隙不消耗配额、门控相位误差阈值 |
| `simulator_node_pacing` | 端到端：任一开放窗口内下发帧数不超过信用上限、信用为 0 时保持旧的一次性抽空行为 |
| `latency_stats` | 最近秩分位数、单样本确定性、相邻绝对差抖动、空摘要归零、优先级 8 以上夹紧、逐 stream 乱序/重复计数、负时延拒绝、蓄水池抽样有界且可复现、`clear` 复位 |
| `scheduler_latency` | 令牌桶按字节精确记账与跨调用累计、速率 0 关闭整形、整形只推迟不丢弃、非法速率拒绝、放行帧按优先级计入时延统计、`offered` 自洽、CSV/JSON/摘要行格式 |
| `native_pubsub_integration` | open62541 Publisher → TsnHub → Subscriber 原生 UDP/UADP 链路（仅在 Linux 原生 PubSub 构建中注册，需要 open62541） |

上表共列出 11 个测试，但 ctest 实际注册的数量取决于配置：

- **有 open62541 的原生构建**（Linux/Docker，见 §六）：11 个全部注册。
- **无 open62541 的 Portable 构建**（`-DTSNHUB_USE_OPEN62541_PUBSUB=OFF`）：`native_pubsub_integration`
  不被注册，实测 `ctest -N` 报告 `Total Tests: 10`（Windows + MinGW 下同样如此）。

Windows 上运行这些测试有一个前提：必须把 WinLibs 的 `mingw64/bin` 目录**前置**到 `PATH`
（例如 `export PATH="/c/Users/Dlut/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"`）。
Git for Windows 自带的 `mingw64/bin`（Git Bash 里是 `/mingw64/bin`，实际路径
`D:/Environment/Git/mingw64/bin`）里也有一份 ABI 不同的 `libstdc++-6.dll`，它默认排在 `PATH` 前面；
若它被加载，测试进程会在 `writeFixture`（`tests/PriorityMapTest.cpp:32`）写临时映射文件时
段错误退出（实测退出码 139，`gdb` 回溯为 `#0..#2 libstdc++-6.dll` → `#3 writeFixture`）。
把 WinLibs 的 bin 前置后同一份可执行文件正常通过；规避方法就是上面的 `PATH` 前置，
或改用完整路径调用 `build-*/tsnhub_*_test.exe` 并同样前置 WinLibs bin。

运行：

```bash
ctest --test-dir build-tsn/cmake --output-on-failure
```

测试断言统一使用 `tests/TestAssertions.hpp` 里的 `TSNHUB_TEST_REQUIRE` /
`TSNHUB_TEST_REQUIRE_MESSAGE`，刻意不复用 `<cassert>`：CMake 的 Release 配置会注入
`-DNDEBUG`，标准断言在该条件下被编译器整体删除，测试会变成"什么都不检查却返回 0"的
假绿灯。这里的宏是普通函数调用，与 `NDEBUG` 无关，并且测试目标额外追加 `-UNDEBUG`
作为双保险；失败时打印 `file:line`、表达式原文与可选详情，并以退出码 1 结束。
每个用例结束时还会打印实际执行的断言条数，便于在 CI 日志中确认检查真的跑过：

```text
Scheduler tests passed (54 assertions)
```

严格编译检查：

```bash
cmake -S . -B build-tsn/warnings \
  -DCMAKE_TOOLCHAIN_FILE=build-tsn/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build build-tsn/warnings -j
ctest --test-dir build-tsn/warnings --output-on-failure
```

Release 构建下的测试同样有效（断言不会被删除）：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

## 十四、安全与限制

- TsnHub 是仿真工具，不提供真实硬件 TSN 的确定性保证。
- 用户态调度会受到操作系统线程调度、容器调度和普通网卡缓冲影响。
- `--loss` 和 `--jitter-us` 使用伪随机算法；固定 `--seed` 可复现实验。
- UDP 不保证可靠、有序或不重复。
- 当前 Portable PubSub Envelope 不是 OPC UA 标准网络格式；标准 UADP 字节位于 payload 内部。
- Linux 原生模式将直接使用 open62541 的标准 UDP/UADP PubSub。

## 十五、许可证

本项目使用 Apache License 2.0，详见 [LICENSE](./LICENSE)。
