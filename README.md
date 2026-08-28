<div align="center">
  <img src="./Logo.png" alt="TsnHub Logo" width="120" />
  <h2>TsnHub</h2>
  <h3>基于 open62541 的 Linux 本地 TSN 仿真中间节点</h3>
</div>

## 一、项目简介

TsnHub 是 FasterEdge 生态中的 TSN 仿真组件。它在 OPC UA PubSub 数据的接收与重新发布之间加入用户态 TSN 调度器，用于在普通 Linux 主机或 Docker 环境中模拟时间敏感网络的队列、优先级、门控、时延、抖动、丢包与拥塞行为。

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
| 固定时延 | 为所有通过节点的帧增加固定转发延迟 |
| 随机抖动 | 在固定时延基础上增加可配置的对称随机抖动 |
| 丢包仿真 | 按 0～1 的概率随机丢弃输入帧 |
| 队列容量 | 限制每个优先级队列的最大积压帧数 |
| 可复现实验 | 通过固定随机种子复现抖动和丢包结果 |
| 运行统计 | 输出 accepted、released、dropped_loss、dropped_queue |
| UDP 转发 | 支持 Linux/macOS BSD Socket 与 Windows Winsock2 |
| open62541 | 链接 open62541 1.5；Linux 原生 PubSub 模式使用 UDP/UADP |

## 四、目录结构

```text
TsnHub/
├── main.cpp                     # CLI 与进程入口
├── tsn/
│   ├── Frame.hpp                # 调度帧定义
│   ├── Scheduler.hpp/.cpp       # 用户态 TSN 调度器
│   └── SimulatorNode.hpp/.cpp   # UDP 中间节点
├── transport/
│   ├── UdpSocket.hpp/.cpp       # 跨平台 UDP 封装
│   └── PortablePubSub.hpp/.cpp  # Portable PubSub Envelope
├── tests/
│   ├── SchedulerTest.cpp
│   ├── PortablePubSubTest.cpp
│   ├── UdpLoopbackTest.cpp
│   └── SimulatorNodeTest.cpp
├── CMakeLists.txt
├── conanfile.py
├── conandata.yml
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
- open62541 1.5

Portable PubSub 模式也可以在 macOS 和 Windows 构建。open62541 原生 UDP/UADP PubSub 模式统一以 Linux/Docker 为目标环境。

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

查看当前构建能力：

```bash
./build-tsn/cmake/TsnHub --capabilities
```

输出示例：

```text
open62541=1.5.0
native_pubsub=false
portable_pubsub=true
portable_scheduler=true
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
| `--seed` | 随机种子 |
| `--capabilities` | 输出构建能力并退出 |

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

## 九、Portable PubSub Envelope

```text
magic      4 bytes  "TSN1"
priority   1 byte   0..7
sequence   8 bytes  big-endian
streamLen  2 bytes  big-endian
stream     N bytes  UTF-8
payload    remaining bytes
```

payload 对 TsnHub 保持透明，可以放入 open62541 或其他 OPC UA 实现生成的 UADP 网络消息。

## 十、Linux 原生 open62541 PubSub

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

## 十一、Docker 三节点仿真

Dockerfile 从源码构建 open62541 1.5.0，并开启：

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

## 十二、测试

| 测试 | 覆盖内容 |
|---|---|
| `scheduler` | Gate、优先级、时延、丢包和队列容量 |
| `portable_pubsub` | Envelope 编解码与畸形包拒绝 |
| `udp_loopback` | 真实 UDP 回环收发与超时 |
| `simulator_node` | UDP 入口 → Scheduler → UDP 出口端到端转发 |
| `native_pubsub_integration` | open62541 Publisher → TsnHub → Subscriber 原生 UDP/UADP 链路 |

运行：

```bash
ctest --test-dir build-tsn/cmake --output-on-failure
```

严格编译检查：

```bash
cmake -S . -B build-tsn/warnings \
  -DCMAKE_TOOLCHAIN_FILE=build-tsn/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build build-tsn/warnings -j
ctest --test-dir build-tsn/warnings --output-on-failure
```

## 十三、安全与限制

- TsnHub 是仿真工具，不提供真实硬件 TSN 的确定性保证。
- 用户态调度会受到操作系统线程调度、容器调度和普通网卡缓冲影响。
- `--loss` 和 `--jitter-us` 使用伪随机算法；固定 `--seed` 可复现实验。
- UDP 不保证可靠、有序或不重复。
- 当前 Portable PubSub Envelope 不是 OPC UA 标准网络格式；标准 UADP 字节位于 payload 内部。
- Linux 原生模式将直接使用 open62541 的标准 UDP/UADP PubSub。

## 十四、许可证

本项目使用 Apache License 2.0，详见 [LICENSE](./LICENSE)。
