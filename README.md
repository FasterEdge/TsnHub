<div align="center">
  <h2>TsnHub</h2>
  <h3>本地 IPC 与 OPC UA 桥接工具</h3>
</div>

TsnHub 是一个将本地 IPC（Unix Socket / Windows NamedPipe / CommandLine）与 OPC UA（open62541）进行桥接的工具，用于 TSN 消息代发代收。

### 一、功能概览

- 三种运行模式：UnixSocket、NamedPipe（Windows）、CommandLine。
- UnixSocket/NamedPipe 支持运行时 CFG 配置 OPC UA 端点与节点。
- CommandLine 模式通过命令行参数配置，并使用 stdin/stdout 交互。
- 真实 open62541 客户端实现：连接、订阅、写入与重连。

### 二、目录结构

- `main.cpp`：入口与模式选择、参数解析。
- `unix_socket/`：Unix Socket 桥接实现。
- `name_pipe/`：Windows NamedPipe 桥接实现。
- `command_line/`：命令行交互实现。
- `tsn/`：open62541 适配层。

### 三、构建依赖

- CMake >= 4.1（由 `CMakeLists.txt` 的 `cmake_minimum_required(VERSION 4.1)` 规定）
- Conan 2.x
- CLI11
- open62541

### 四、构建

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j
```

### 五、运行模式与默认值

- 未传 `--mode` 时：
  - Windows 默认 `NamedPipe`，管道名 `\\.\pipe\tsn_hub_service`
  - 非 Windows 默认 `UnixSocket`，路径 `/var/run/tsn_hub_service.sock`
- 命令行参数优先于平台默认。

#### UnixSocket 模式

```bash
./cmake-build-debug/TsnHub -m UnixSocket -a /tmp/tsn_hub.sock
```

#### Windows NamedPipe 模式

```bash
TsnHub.exe -m NamedPipe -a tsn_hub_pipe
```

> 注：可以传入完整管道名（如 `\\.\pipe\tsn_hub_service`），或只传管道短名。

#### CommandLine 模式

```bash
./cmake-build-debug/TsnHub -m CommandLine \
  --endpoint opc.tcp://127.0.0.1:4840 \
  --tx 2:TsnTx \
  --rx 2:TsnRx
```

- stdin 每行作为一条上行消息发送到 `tx` 节点。
- 订阅到的 `rx` 数据会输出到 stdout。

### 六、CFG 配置协议（UnixSocket / NamedPipe）

配置行格式（单行）：

```
CFG endpoint=opc.tcp://127.0.0.1:4840 tx=2:TsnTx rx=2:TsnRx
```

示例（UnixSocket）：

```bash
printf 'CFG endpoint=opc.tcp://127.0.0.1:4840 tx=2:TsnTx rx=2:TsnRx\n' | nc -U /tmp/tsn_hub.sock
```

### 七、常见问题

- `bind 失败: Permission denied`：
  - `/var/run` 可能需要管理员权限，建议改用 `/tmp` 或提升权限。

- `BadConnectionRejected` / `Could not open a TCP connection`：
  - 确认本机 `opc.tcp://127.0.0.1:4840` 有可用 OPC UA 服务。
  - 检查防火墙或端口占用。

### 八、许可证

请按项目仓库中的许可协议使用。
