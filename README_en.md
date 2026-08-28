<div align="center">
  <img src="./Logo.png" alt="TsnHub Logo" width="120" />
  <h2>TsnHub</h2>
  <h3>A Linux Local TSN Simulation Intermediate Node Based on open62541</h3>
</div>

## 1. Introduction

TsnHub is the TSN simulation component of the FasterEdge ecosystem. It inserts a user-space TSN scheduler between the reception and re-publication of OPC UA PubSub data, used to simulate the queue, priority, gating, delay, jitter, packet loss and congestion behavior of time-sensitive networks on ordinary Linux hosts or in Docker environments.

Target data flow:

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

The current repository already implements the Linux-native open62541 UDP/UADP DataSetReader/DataSetWriter, a user-space TSN scheduler, a Portable PubSub compatibility mode, and a Publisher → TsnHub → Subscriber three-node Docker simulation environment.

## 2. Terminology

- **Frame**: an incoming message awaiting scheduling in TsnHub, containing sequence, priority, stream and payload.
- **Priority Queue**: eight independent queues with priorities 0–7, where 7 is the highest priority.
- **Gate Control List (GCL)**: a time-slot list controlling which priority queues may send during each period.
- **Portable PubSub**: a cross-platform UDP wrapper whose payload can transparently carry OPC UA PubSub/UADP network messages.
- **Native PubSub**: UDP/UADP send/receive on Linux done by open62541's native DataSetReader/DataSetWriter.
- **SimulatorNode**: the intermediate node connecting the ingress, scheduler and egress.

## 3. Core Capabilities

| Capability | Description |
|---|---|
| Eight priority levels | Supports priorities 0–7 and releases high-priority frames first |
| Gate Control List | Uses repeating time slots to define open windows for different priority queues |
| Fixed delay | Adds a fixed forwarding delay to all frames passing through the node |
| Random jitter | Adds configurable symmetric random jitter on top of the fixed delay |
| Packet loss simulation | Randomly drops incoming frames with a configurable probability between 0 and 1 |
| Queue capacity | Limits the maximum number of backed-up frames in each priority queue |
| Reproducible experiments | Reproduces jitter and loss results via a fixed random seed |
| Runtime statistics | Outputs accepted, released, dropped_loss, dropped_queue |
| UDP forwarding | Supports Linux/macOS BSD sockets and Windows Winsock2 |
| open62541 | Links open62541 1.5; native PubSub mode on Linux uses UDP/UADP |

## 4. Directory Structure

```text
TsnHub/
├── main.cpp                     # CLI and process entry point
├── tsn/
│   ├── Frame.hpp                # Scheduling frame definition
│   ├── Scheduler.hpp/.cpp       # User-space TSN scheduler
│   └── SimulatorNode.hpp/.cpp   # UDP intermediate node
├── transport/
│   ├── UdpSocket.hpp/.cpp       # Cross-platform UDP wrapper
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

Native PubSub and Docker files:

```text
├── pubsub/Open62541Bridge.*     # open62541 DataSetReader/DataSetWriter
├── docker/PubSubFixture.cpp     # Publisher / Subscriber test programs
├── Dockerfile                   # open62541 source build and runtime image
├── docker-compose.yml           # Three-node simulation topology
└── tests/native_pubsub_integration.sh
```

## 5. Build Dependencies

- Linux (final runtime environment for Native PubSub)
- CMake >= 3.23
- Conan 2.x
- C++17 compiler
- CLI11 2.6
- open62541 1.5

Portable PubSub mode can also be built on macOS and Windows. The open62541 native UDP/UADP PubSub mode targets Linux/Docker as the runtime environment.

## 6. Local Build

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

View the current build capabilities:

```bash
./build-tsn/cmake/TsnHub --capabilities
```

Example output:

```text
open62541=1.5.0
native_pubsub=false
portable_pubsub=true
portable_scheduler=true
```

In a Linux Native PubSub build, `native_pubsub` should be `true`.

## 7. Running in Portable PubSub Mode

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

Argument reference:

| Argument | Description |
|---|---|
| `--listen host:port` | Ingress UDP address |
| `--forward host:port` | Egress UDP address |
| `--gate duration_us:mask` | Gate time slot, repeatable; mask bits 0–7 correspond to priorities 0–7 |
| `--delay-us` | Fixed forwarding delay in microseconds |
| `--jitter-us` | Random jitter range; the actual value is ±jitter |
| `--loss` | Random packet loss probability between 0 and 1 |
| `--queue` | Maximum queued frames per priority |
| `--seed` | Random seed |
| `--capabilities` | Prints build capabilities and exits |

After pressing `Ctrl+C` or sending `SIGTERM`, the node stops and prints statistics.

## 8. Gate Configuration Example

```bash
--gate 1000:0x0f --gate 1000:0xf0
```

Represents a 2000-microsecond period:

1. The first 1000 microseconds open priorities 0–3;
2. The next 1000 microseconds open priorities 4–7.

If `--gate` is not provided, all priorities are open every 1000 microseconds by default:

```text
1000:0xff
```

## 9. Portable PubSub Envelope

```text
magic      4 bytes  "TSN1"
priority   1 byte   0..7
sequence   8 bytes  big-endian
streamLen  2 bytes  big-endian
stream     N bytes  UTF-8
payload    remaining bytes
```

The payload is transparent to TsnHub; it can hold UADP network messages generated by open62541 or other OPC UA implementations.

## 10. Linux Native open62541 PubSub

Linux native mode uses the following open62541 components:

- `UA_PubSubConnectionConfig`
- `UA_ReaderGroupConfig`
- `UA_DataSetReaderConfig`
- `UA_Server_DataSetReader_createTargetVariables`
- `UA_PublishedDataSetConfig`
- `UA_DataSetFieldConfig`
- `UA_WriterGroupConfig`
- `UA_DataSetWriterConfig`

Expected processing chain:

```text
DataSetReader subscribed variable
              |
              v
         TSN Scheduler
              |
              v
DataSetWriter published variable
```

The first native DataSet uses `Int32` fields to ease verification of the full Publisher → TsnHub → Subscriber chain.

Native mode run example:

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

## 11. Docker Three-Node Simulation

The Dockerfile builds open62541 1.5.0 from source with:

```text
UA_ENABLE_PUBSUB=ON
UA_ENABLE_PUBSUB_INFORMATIONMODEL=ON
```

Build the image:

```bash
docker build -t tsnhub .
```

Start Publisher, TsnHub and Subscriber:

```bash
docker compose up --build --abort-on-container-exit
```

Expected results include:

```text
publisher final_value=20
accepted=20 released=20 dropped_loss=0 dropped_queue=0
subscriber final_value=20
```

Stop and clean up:

```bash
docker compose down --remove-orphans
```

## 12. Tests

| Test | Coverage |
|---|---|
| `scheduler` | Gate, priority, delay, packet loss and queue capacity |
| `portable_pubsub` | Envelope encoding/decoding and malformed packet rejection |
| `udp_loopback` | Real UDP loopback send/receive and timeout |
| `simulator_node` | End-to-end forwarding from UDP ingress → Scheduler → UDP egress |
| `native_pubsub_integration` | open62541 Publisher → TsnHub → Subscriber native UDP/UADP chain |

Run:

```bash
ctest --test-dir build-tsn/cmake --output-on-failure
```

Strict compile checks:

```bash
cmake -S . -B build-tsn/warnings \
  -DCMAKE_TOOLCHAIN_FILE=build-tsn/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build build-tsn/warnings -j
ctest --test-dir build-tsn/warnings --output-on-failure
```

## 13. Security and Limitations

- TsnHub is a simulation tool; it does not provide the deterministic guarantees of real hardware TSN.
- User-space scheduling is affected by OS thread scheduling, container scheduling and ordinary NIC buffering.
- `--loss` and `--jitter-us` use a pseudo-random algorithm; a fixed `--seed` reproduces experiments.
- UDP does not guarantee reliability, ordering or deduplication.
- The current Portable PubSub Envelope is not a standard OPC UA network format; standard UADP bytes live inside the payload.
- Linux native mode directly uses open62541's standard UDP/UADP PubSub.

## 14. License

This project is licensed under the Apache License 2.0; see [LICENSE](./LICENSE).