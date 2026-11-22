<div align="center">
  <img src="./Logo.png" alt="TsnHub Logo" width="120" />
  <h2>TsnHub</h2>
  <h3>A Linux Local TSN Simulation Intermediate Node Based on open62541</h3>
</div>

## 1. Introduction

TsnHub is the TSN simulation component of the FasterEdge ecosystem. It inserts a user-space TSN scheduler between the reception and re-publication of OPC UA PubSub data, used to simulate the queue, priority, gating, delay, jitter, packet loss and congestion behavior of time-sensitive networks on ordinary Linux hosts or in Docker environments.

Every one of those behaviours is measurable: latency and jitter are reported per priority as p50/p99/max (see "9. Latency and Congestion Measurement"), and congestion is implemented as the two-level rate limit of `--shape-mbps` token bucket plus `--credit-frames-per-window` window quota. Note the two distinct loss definitions: `loss_rate = dropped_loss / offered` only reports the random loss set by `--loss` (`tsn/Scheduler.cpp:69`) and is independent of load, whereas the load-dependent figure is `drop_rate = (dropped_loss + dropped_queue) / offered`, because `dropped_queue` comes from queue-capacity overflow. A loss-versus-load curve must therefore plot `drop_rate` against `offered`.

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
| Absolute-time gate wake-up | Wakes at the absolute GCL boundary (`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` on Linux, a high-resolution waitable timer on Windows), decoupling gate phase from polling granularity |
| Paced dequeue (credit) | Frames released per open GCL window are capped by `--credit-frames-per-window`; the excess is deferred to the next window instead of draining the whole backlog at once |
| Egress shaping (token bucket) | Serialises frames into a byte stream at the `--shape-mbps` line rate; a token shortage defers instead of dropping |
| Fixed delay | Adds a fixed forwarding delay to all frames passing through the node |
| Random jitter | Adds configurable symmetric random jitter on top of the fixed delay |
| Packet loss simulation | Randomly drops incoming frames with a configurable probability between 0 and 1 |
| Queue capacity | Limits the maximum number of backed-up frames in each priority queue |
| Reproducible experiments | Reproduces jitter and loss results via a fixed random seed |
| Runtime statistics | Outputs accepted, released, dropped_loss, dropped_queue, shaped_deferrals, credit_deferrals |
| End-to-end latency statistics | Per-priority p50/p90/p99/max/mean latency, jitter and reordering; exportable as CSV/JSON |
| UDP forwarding | Supports Linux/macOS BSD sockets and Windows Winsock2 |
| open62541 | Links open62541 1.5.9; native PubSub mode on Linux uses UDP/UADP |

## 4. Directory Structure

```text
TsnHub/
├── main.cpp                     # CLI and process entry point
├── tsn/
│   ├── Frame.hpp                # Scheduling frame definition
│   ├── GateClock.hpp/.cpp       # GCL phase computation and absolute-time sleep (cross-platform)
│   ├── LatencyStats.hpp/.cpp    # End-to-end latency / jitter / reordering statistics (deterministic time source)
│   ├── PriorityMap.hpp/.cpp     # PublisherId/WriterGroupId/DataSetWriterId -> priority mapping
│   ├── Scheduler.hpp/.cpp       # User-space TSN scheduler
│   ├── StatsReport.hpp/.cpp     # Statistics output (summary line / percentile table / CSV / JSON)
│   └── SimulatorNode.hpp/.cpp   # UDP intermediate node
├── transport/
│   ├── UdpSocket.hpp/.cpp       # Cross-platform UDP wrapper
│   └── PortablePubSub.hpp/.cpp  # Portable PubSub Envelope
├── tests/
│   ├── SchedulerTest.cpp
│   ├── SchedulerPacingTest.cpp  # Per-window frame cap, credit reset, gate phase error
│   ├── SimulatorNodePacingTest.cpp # End-to-end window pacing over a real UDP loopback
│   ├── SchedulerLatencyTest.cpp # Token-bucket accounting, percentile/CSV/JSON output, drop counters
│   ├── GateClockTest.cpp       # Slot phase table, boundary instants, absolute sleep accuracy
│   ├── LatencyStatsTest.cpp    # Latency / jitter / reordering statistics with an injected deterministic time source
│   ├── PortablePubSubTest.cpp
│   ├── PriorityMapTest.cpp
│   ├── UdpLoopbackTest.cpp
│   ├── SimulatorNodeTest.cpp
│   └── TestAssertions.hpp         # NDEBUG-independent test assertions
├── CMakeLists.txt
├── conanfile.py
├── conandata.yml
├── conan_provider.cmake        # Conan provider bridge for CMake (with `conan install --build=missing`)
├── SECURITY.md                 # Security policy and vulnerability reporting channel
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
- open62541 1.5.9

Portable PubSub mode can also be built on macOS and Windows. The open62541 native UDP/UADP PubSub mode targets Linux/Docker as the runtime environment.

One prerequisite for running the unit tests on Windows: Git for Windows ships its own
`mingw64/bin` (`/mingw64/bin` inside Git Bash, i.e. `D:/Environment/Git/mingw64/bin`) with an
ABI-incompatible `libstdc++-6.dll` that comes earlier in `PATH` by default. If that one is
loaded before the WinLibs runtime, the test binaries crash with a segmentation fault. The
WinLibs `mingw64/bin` directory must therefore come **first** in `PATH`:

```bash
export PATH="/c/Users/Dlut/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"
```

The MinGW toolchain in that same directory is also what the build itself uses
(`cmake -G "MinGW Makefiles"`).

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

Without conan / open62541 / CLI11, the portable mode can still be configured and tested
on its own:

```bash
cmake -S . -B build-portable -DTSNHUB_USE_OPEN62541_PUBSUB=OFF
cmake --build build-portable -j
ctest --test-dir build-portable --output-on-failure
```

In that configuration the `TsnHub` CLI executable and the native PubSub targets are
skipped, and the configure step prints `CLI11 not found` / `open62541 not found` to say
why. Neither `tsnhub_core` nor the unit tests depend on those packages, so they always
build. Requesting `TSNHUB_USE_OPEN62541_PUBSUB=ON` without open62541 present is a hard
configure error instead of a silent downgrade to portable mode.

**Without open62541 (or without CLI11) there is no `TsnHub` executable at all, so the
measurement commands in §9 that rely on `--stats-format` / `--stats-file` / `--run-ms`
cannot run either.** The reason is structural: `main.cpp:8` unconditionally
`#include <open62541/config.h>` (needed for the `--capabilities` version output and the
native URL handling), and `CMakeLists.txt:66` guards the target with
`if(CLI11_FOUND AND open62541_FOUND)`, so both packages must be present. In that state only
`tsnhub_core` and the 10 unit tests (§13) can be verified; producing CSV/JSON datasets
requires open62541 and CLI11 to be installed first, via the conan flow in §6 or the Docker
image in §12.

View the current build capabilities:

```bash
./build-tsn/cmake/TsnHub --capabilities
```

Example output:

```text
open62541=1.5.9
native_pubsub=false
portable_pubsub=true
portable_scheduler=true
latency_stats=true
egress_shaping=true
stats_formats=summary,table,csv,json
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
  --credit-frames-per-window 16 \
  --shape-mbps 100 \
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
| `--shape-mbps` | Egress token-bucket shaping rate in Mbit/s (0 disables shaping) |
| `--shape-burst-bytes` | Token bucket depth in bytes (0 uses the 64 KiB default) |
| `--credit-frames-per-window` | Maximum frames released per open GCL window (0 disables window pacing and keeps the legacy drain-all behaviour) |
| `--stats-format` | Statistics output format: `summary` (default), `table`, `csv` or `json` |
| `--stats-file` | Writes statistics to this file; without it everything goes to stdout |
| `--run-ms` | Stops after this many milliseconds and prints statistics (0 runs until SIGINT/SIGTERM) |
| `--seed` | Random seed |
| `--capabilities` | Prints build capabilities and exits |

Native-mode arguments (`--mode native`, needs open62541 + CLI11):

| Argument | Description |
|---|---|
| `--mode` | Transport mode: `portable` (default) or `native` |
| `--subscribe-url` | Native-mode input URL (`opc.udp://0.0.0.0:4841`) |
| `--publish-url` | Native-mode output URL |
| `--publisher-bind-url` | **Local bind** URL of the native writer; open62541 ≥ 1.5 rejects UDP port 0, so this must be a fixed port (`opc.udp://0.0.0.0:4843` by default) distinct from `--subscribe-url` and from any co-located subscriber's bind port |
| `--input-publisher-id` | Expected upstream PublisherId |
| `--input-writer-group-id` | Expected upstream WriterGroupId |
| `--input-writer-id` | Expected upstream DataSetWriterId |
| `--output-publisher-id` | TsnHub output PublisherId |
| `--output-writer-group-id` | TsnHub output WriterGroupId |
| `--output-writer-id` | TsnHub output DataSetWriterId |
| `--publish-interval-ms` | Native-mode publishing interval in milliseconds |
| `--priority-rule` | Native-mode priority rule `publisher:writerGroup:writer:priority`, repeatable; a single component may be `*`, but a rule wildcard in all three components is rejected |
| `--priority-map-file` | Plain-text file with one priority rule per line, `#` comments and blank lines allowed |
| `--input-default-priority` | Priority used when no rule matches (default 0, range 0–7) |

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

## 9. Latency and Congestion Measurement

`Frame.receivedAt` is stamped at enqueue time and subtracted from the current time at dequeue,
giving the **node residency latency**. `tsn/LatencyStats.cpp` accumulates it per priority into
p50/p90/p99/max/mean plus jitter, and counts sequence regressions (reordering) and repeats
within the same stream.

The time source is passed in explicitly as a `std::chrono::steady_clock::time_point`; the
statistics never read a real clock, so the same input always yields the same output. Unit
tests therefore assert percentiles bit-for-bit using an injected deterministic time source.

Percentiles use nearest-rank: the q-th percentile is the `ceil(q*n)`-th smallest sample
(1-based), so with `n=10` p50 is the 5th smallest and p99 is the 10th (the maximum). Once a
priority exceeds its retention bound (`SchedulerConfig::latencyRetention`, 16384 by default)
a fixed-seed reservoir sample takes over, bounding memory; the `retained` field then reports
how many samples actually contributed to the percentiles, while `samples` stays the count of
all frames.

At the end of a run `--stats-format` selects the output (these switches live on the `TsnHub`
CLI, which needs open62541 + CLI11 to build — see §6):

| Format | Content |
|---|---|
| `summary` | Single-line counters (default). Keeps the existing `accepted=/released=/dropped_loss=/dropped_queue=` prefix and adds `shaped_deferrals=`, `credit_deferrals=` and `latency_samples=` |
| `table` | Human-readable per-priority percentile table |
| `csv` | One row per priority with min/p50/p90/p99/max/mean/jitter, reordering, duplicates, offered and loss_rate/drop_rate |
| `json` | The same source data as CSV in JSON form: `totals` carries accepted/released/dropped_loss/dropped_queue/shaped_deferrals/credit_deferrals/latency_samples, the `priorities` array carries every per-priority CSV field, and the `note` field echoes the run configuration |

In the CSV, `offered = accepted + dropped_loss + dropped_queue`; together with `loss_rate`
and `drop_rate` this is the minimal sufficient statistic for a loss-rate versus load curve.
The two columns measure different things: `loss_rate = dropped_loss / offered` counts only
the random loss set by `--loss`, whose expected value is `--loss` itself and is independent
of load, while `drop_rate = (dropped_loss + dropped_queue) / offered` also includes
queue-capacity overflow (`dropped_queue`) and therefore rises with load. A loss-versus-load
curve must use `offered` as the x axis and `drop_rate` as the y axis; plotting `loss_rate`
yields a flat line at `--loss` and does not reflect congestion.

`--stats-file` redirects the statistics to a file so one experiment produces one dataset;
without it everything goes to stdout.

`--run-ms` makes the node stop on its own after a fixed window and flush the statistics:
under Windows, MSYS `kill` only performs `TerminateProcess`, so the process never sees a
signal and the statistics never reach disk. With a window length, a scripted run produces a
complete dataset with or without a signal channel.

```bash
./build-release/TsnHub \
  --listen 127.0.0.1:4841 --forward 127.0.0.1:4842 \
  --shape-mbps 100 --delay-us 200 --loss 0.01 \
  --stats-format csv --stats-file run-100m.csv --run-ms 60000
```

Congestion is implemented as two levels of rate limiting. `--shape-mbps` drives a token
bucket that serialises dequeued frames into a byte stream at the line rate; a token shortage
defers and counts `shaped_deferrals`. `--credit-frames-per-window` additionally caps the
frames released inside a single GCL window, deferring the excess to the next window and
counting `credit_deferrals`. Both only defer, never drop, and deferred frames still occupy
queue capacity.

## 10. Portable PubSub Envelope

```text
magic      4 bytes  "TSN1"
priority   1 byte   0..7
sequence   8 bytes  big-endian
streamLen  2 bytes  big-endian
stream     N bytes  UTF-8
payload    remaining bytes
```

The payload is transparent to TsnHub; it can hold UADP network messages generated by open62541 or other OPC UA implementations.

## 11. Linux Native open62541 PubSub

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

`--publisher-bind-url` must not be omitted: open62541 treats the PubSubConnection URL as the
writer's local bind address (`pubsub/Open62541Bridge.cpp:161`), and open62541 ≥ 1.5 rejects
UDP port 0. Without it the writer falls back to an ephemeral port and, on some versions, the
connection cannot be created at all; pinning it to a fixed port distinct from
`--subscribe-url` is what makes the three-node chain reproducible.

### Native-mode priority mapping

In native mode, the priority of every inbound stream is decided by its
`(PublisherId, WriterGroupId, DataSetWriterId)` triple, configured through
`--priority-rule` (repeatable) or `--priority-map-file` (one rule per line).
The file passed to `--priority-map-file` is plain text and accepts `#` line comments
and blank lines (`PriorityMap::loadFile` strips the `#` comment before parsing each
line), so rules can be grouped per stream with an explanation:

```bash
TsnHub --mode native \
  --input-publisher-id 1 --input-writer-group-id 1 --input-writer-id 1 \
  --priority-rule 1:1:1:6 \
  --gate 1000:0x40
```

The equivalent map file (everything after `#` is stripped when the file is loaded):

```text
# 1:1:1:6
1:1:1:6
```

A rule reads `publisher:writerGroup:writer:priority`, with `priority` in 0–7 (7 is
highest). Any one of the three components may be `*` for a wildcard; the matching rule is
the one with the most non-wildcard components. Two rules that can match the same stream
at equal specificity are rejected at load time, so parsing stays deterministic. A rule
that is wildcard in all three components (`*:*:*:p`) is rejected as well
(`PriorityMap::add` requires at least one concrete component) — to give every stream the
same priority, use `--input-default-priority` instead.
When no rule matches, `--input-default-priority` (default 0) applies.

Why the mapping exists: open62541's DataSetReader only accepts network messages
whose triple matches the configured one, so that triple is the frame's identity.
Earlier builds hard-coded `frame.priority = 0`, which packed every frame into
queue 0 and made only bit0 of a `--gate` mask meaningful — the eight priority
queues and the GCL were fully degraded on the native path. The following
comparison reproduces the issue on a native build:

| GCL | Priority rule | Subscriber outcome |
|---|---|---|
| `1000:0xff` | none | `subscriber final_value=20` |
| `1000:0x40` | `--priority-rule 1:1:1:6` | `subscriber final_value=20` |
| `1000:0x40` | none (default priority 0) | `subscriber final_value=0` |

The last row is the degradation itself: the GCL opens only priority 6, while the
frame is still pinned to queue 0, so nothing is released at all.

### Gate wake-up and paced dequeue

The previous implementation drove the gate with a `sleep_for(1ms)` polling loop,
so slots shorter than 1 ms were resolved by the polling granularity rather than
the clock; `releaseReady` also drained every eligible frame in one call, which
is a burst rather than TAS paced transmission. Both paths now change:

1. **Absolute-time wake-up**: `sleepUntilMonotonic` in `tsn/GateClock.cpp` uses
   `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` on Linux and
   `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)` on
   Windows, isolated by the `_WIN32` macro. `SimulatorNode::run` sleeps until the
   **absolute boundary** at which the current GCL window ends, and the ingress
   wait is clamped to the same boundary, so the gate phase does not accumulate
   scheduling delay.
2. **Paced dequeue (credit)**: `SchedulerConfig::creditFramesPerWindow` caps the
   frames released in a single open window. The excess stays queued and is
   deferred to the next window, counted as `credit_deferrals`. The credit is
   shared per window (a high priority cannot take a low priority's quota), and
   closed slots do not consume credit by default. Together with the `--shape-mbps`
   token bucket this forms a two-level "window quota + line rate" constraint that
   only defers, never drops. `creditFramesPerWindow = 0` keeps the legacy
   drain-all behaviour.

Covered by `gate_clock` (phase table and wake-up accuracy: never before the
deadline, overshoot within one scheduler tick) and `scheduler_pacing`
(per-window frame cap, credit reset per window, closed slots do not consume
credit, gate phase error within threshold).

## 12. Docker Three-Node Simulation

The Dockerfile builds open62541 1.5.9 from source with:

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

## 13. Tests

| Test | Coverage |
|---|---|
| `scheduler` | Gate, priority, delay, packet loss and queue capacity |
| `portable_pubsub` | Envelope encoding/decoding and malformed packet rejection |
| `udp_loopback` | Real UDP loopback send/receive and timeout |
| `simulator_node` | End-to-end forwarding from UDP ingress → Scheduler → UDP egress |
| `priority_map` | `PublisherId:WriterGroupId:WriterId:priority` rule parsing, wildcards, ambiguity rejection, and the mapped priority actually driving the GCL |
| `gate_clock` | GCL slot phase table, absolute window boundary, and wake-up accuracy of absolute-time sleep |
| `scheduler_pacing` | Frames per window never exceed the credit cap, credit resets per window, closed slots do not consume credit, gate phase error within threshold |
| `simulator_node_pacing` | End to end: frames per open window never exceed the credit cap; credit 0 keeps the legacy drain-all behaviour |
| `latency_stats` | Nearest-rank percentiles, single-sample determinism, mean absolute neighbour delta as jitter, zeroed empty summary, priority above 7 clamped, per-stream reordering/duplicate counting, negative latency rejected, bounded and reproducible reservoir retention, `clear` reset |
| `scheduler_latency` | Token bucket charged byte-exactly and accruing across calls, rate 0 disabling shaping, shaping deferring without dropping, invalid rate rejected, released frames measured per priority, `offered` self-consistency, CSV/JSON/summary formats |
| `native_pubsub_integration` | open62541 Publisher → TsnHub → Subscriber native UDP/UADP chain (registered only in a Linux native PubSub build with open62541 present) |

The table lists 11 tests, but the number ctest actually registers depends on the configuration:

- **Native build with open62541** (Linux/Docker, see §6): all 11 are registered.
- **Portable build without open62541** (`-DTSNHUB_USE_OPEN62541_PUBSUB=OFF`): `native_pubsub_integration`
  is not registered, and `ctest -N` reports `Total Tests: 10` (measured on Windows + MinGW too).

Running these tests on Windows has one prerequisite: the WinLibs `mingw64/bin` directory must
come **first** in `PATH`
(e.g. `export PATH="/c/Users/Dlut/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"`).
Git for Windows ships its own `mingw64/bin` (`/mingw64/bin` inside Git Bash, i.e.
`D:/Environment/Git/mingw64/bin`) with an ABI-incompatible `libstdc++-6.dll` that comes earlier
in `PATH` by default. If that one is loaded, the test binary dies with a segmentation fault
(measured exit code 139; `gdb` backtrace `#0..#2 libstdc++-6.dll` → `#3 writeFixture`) while
writing its temporary map file in `writeFixture` (`tests/PriorityMapTest.cpp:32`). Prepending
the WinLibs bin directory makes the same executable pass. The workaround is that `PATH` prefix,
or invoking `build-*/tsnhub_*_test.exe` by full path with the same prefix.

Run:

```bash
ctest --test-dir build-tsn/cmake --output-on-failure
```

Test assertions live in `tests/TestAssertions.hpp` and use `TSNHUB_TEST_REQUIRE` /
`TSNHUB_TEST_REQUIRE_MESSAGE` on purpose: CMake's Release configuration injects
`-DNDEBUG`, under which standard `assert` is deleted by the compiler and the tests turn
into a false green that returns 0 without checking anything. These macros are plain
function calls and therefore independent of `NDEBUG`; the test targets additionally append
`-UNDEBUG` as a second line of defence. On failure they print `file:line`, the expression
text and an optional detail, then exit with status 1. Each case also prints the number of
assertions it actually executed, so CI logs show that the checks really ran:

```text
Scheduler tests passed (54 assertions)
```

Strict compile checks:

```bash
cmake -S . -B build-tsn/warnings \
  -DCMAKE_TOOLCHAIN_FILE=build-tsn/build/Debug/generators/conan_toolchain.cmake \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build build-tsn/warnings -j
ctest --test-dir build-tsn/warnings --output-on-failure
```

Release builds keep the tests effective (assertions are not deleted):

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

## 14. Security and Limitations

- TsnHub is a simulation tool; it does not provide the deterministic guarantees of real hardware TSN.
- User-space scheduling is affected by OS thread scheduling, container scheduling and ordinary NIC buffering.
- `--loss` and `--jitter-us` use a pseudo-random algorithm; a fixed `--seed` reproduces experiments.
- UDP does not guarantee reliability, ordering or deduplication.
- The current Portable PubSub Envelope is not a standard OPC UA network format; standard UADP bytes live inside the payload.
- Linux native mode directly uses open62541's standard UDP/UADP PubSub.

## 15. License

This project is licensed under the Apache License 2.0; see [LICENSE](./LICENSE).