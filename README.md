# TsnHub

Unix Socket to OPC UA (open62541) bridge with runtime configuration over the socket.

## Build

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j
```

## Run

```bash
./cmake-build-debug/TsnHub -m UnixSocket -a /tmp/tsn_hub.sock
```

## Configure OPC UA at runtime

Send a single-line config message to the Unix socket:

```bash
printf 'CFG endpoint=opc.tcp://127.0.0.1:4840 tx=2:TsnTx rx=2:TsnRx\n' | nc -U /tmp/tsn_hub.sock
```

## Send a TSN payload

```bash
printf 'hello-tsn\n' | nc -U /tmp/tsn_hub.sock
```
