<div align="center">
  <h2>TsnHub</h2>
  <h3>基于 open62541 的跨平台本地 TSN 仿真中间节点</h3>
</div>

TsnHub 在两个 UDP 端点之间转发 OPC UA PubSub 网络消息，并在用户态模拟 TSN 的队列、优先级、Gate Control List、固定时延、抖动、丢包和队列溢出。消息 payload 对中间节点保持透明，可承载 open62541 或其他 OPC UA 栈生成的 UADP 字节。

## 架构

```text
OPC UA PubSub/UADP producer
            |
       UDP input
            v
 +-------------------------+
 | Portable PubSub envelope|
 | stream/sequence/priority|
 +-------------------------+
            |
 +-------------------------+
 | User-space TSN scheduler|
 | 8 queues / GCL / delay  |
 | jitter / loss / capacity|
 +-------------------------+
            |
       UDP output
            v
OPC UA PubSub/UADP consumer
```

跨平台默认实现使用 BSD Socket / Winsock。Linux 环境可以另外编译启用了 `UA_ENABLE_PUBSUB` 的 open62541，并在后续适配原生 PubSub；Conan Center 的 open62541 1.5.0 配方目前将 PubSub 选项限制为 Linux，因此 macOS/Windows 默认使用 Portable PubSub UDP 传输。

## 依赖

- CMake >= 3.23
- Conan 2.x
- C++17 编译器
- CLI11 2.6
- open62541 1.5

## 构建

```bash
conan install . --output-folder=build-tsn --build=missing -s build_type=Debug
cmake -S . -B build-tsn/cmake \
  -DCMAKE_TOOLCHAIN_FILE=build-tsn/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsn/cmake -j
ctest --test-dir build-tsn/cmake --output-on-failure
```

## 运行中间节点

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

参数：

- `--listen host:port`：入口 UDP 地址。
- `--forward host:port`：出口 UDP 地址。
- `--gate duration_us:mask`：可重复指定的 Gate 时隙。bit 0～7 对应优先级 0～7。
- `--delay-us`：固定转发延迟。
- `--jitter-us`：延迟的对称随机抖动范围。
- `--loss`：0～1 的随机丢包率。
- `--queue`：每个优先级的最大排队帧数。
- `--seed`：随机种子，用于复现实验。
- `--capabilities`：显示 open62541 和传输能力。

按 Ctrl+C 或发送 SIGTERM 后，节点停止并打印统计：accepted、released、dropped_loss、dropped_queue。

## Portable PubSub Envelope

中间节点接收的 UDP 数据格式为：

```text
magic      4 bytes  "TSN1"
priority   1 byte   0..7
sequence   8 bytes  big-endian
streamLen  2 bytes  big-endian
stream     N bytes  UTF-8
payload    remaining bytes (opaque OPC UA PubSub/UADP network message)
```

中间节点只调度 envelope，不解析或修改 UADP payload。

## 能力检测

```bash
./build-tsn/cmake/TsnHub --capabilities
```

示例：

```text
open62541=1.5.0
native_pubsub=false
portable_pubsub=true
portable_scheduler=true
```

## 测试

- `scheduler`：Gate、严格优先级、时延、丢包和队列容量。
- `portable_pubsub`：Envelope 编解码和畸形包拒绝。
- `udp_loopback`：真实 UDP 回环收发和超时。
- `simulator_node`：入口 UDP → Scheduler → 出口 UDP 的端到端转发、优先级和时延。

## 平台说明

- Linux：BSD socket；可额外构建 open62541 原生 PubSub。
- macOS：BSD socket Portable PubSub。
- Windows：Winsock2 Portable PubSub，CMake 自动链接 `ws2_32`。

## 许可证

Apache License 2.0，详见 `LICENSE`。
