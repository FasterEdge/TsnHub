#!/bin/sh
# ─────────────────────────────────────────────────────────────
# FasterEdge 开源项目
# Github: https://github.com/FasterEdge
# Gitee:  https://gitee.com/FasterEdge
# ─────────────────────────────────────────────────────────────
set -eu

BIN_DIR=${1:-.}
BASE_PORT=${TSNHUB_TEST_BASE_PORT:-4851}
INPUT_PORT=$BASE_PORT
OUTPUT_PORT=$((BASE_PORT + 1))
# open62541 >= 1.5.x rejects a UDP port-0 connection URL, so each publisher needs
# an explicit non-zero local bind that stays clear of the two subscriber ports.
HUB_BIND_PORT=$((BASE_PORT + 2))
FIXTURE_BIND_PORT=$((BASE_PORT + 3))
TMP=${TMPDIR:-/tmp}/tsnhub-native-$$
mkdir -p "$TMP"
HUB_PID=""
SUB_PID=""
cleanup() {
  status=$?
  if [ "$status" -ne 0 ]; then
    echo "Native PubSub integration failed with exit status $status; process logs:"
    for log in publisher.log subscriber.log hub.log; do
      echo "--- $log ---"
      cat "$TMP/$log" 2>/dev/null || true
    done
  fi
  [ -z "$HUB_PID" ] || kill -TERM "$HUB_PID" 2>/dev/null || true
  [ -z "$SUB_PID" ] || kill -TERM "$SUB_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

"$BIN_DIR/tsnhub_pubsub_fixture" subscriber \
  --url "opc.udp://0.0.0.0:$OUTPUT_PORT" \
  --publisher-id 2 --writer-group-id 2 --writer-id 2 --expect 20 \
  >"$TMP/subscriber.log" 2>&1 &
SUB_PID=$!

"$BIN_DIR/TsnHub" --mode native \
  --subscribe-url "opc.udp://0.0.0.0:$INPUT_PORT" \
  --publish-url "opc.udp://127.0.0.1:$OUTPUT_PORT" \
  --publisher-bind-url "opc.udp://0.0.0.0:$HUB_BIND_PORT" \
  --input-publisher-id 1 --input-writer-group-id 1 --input-writer-id 1 \
  --output-publisher-id 2 --output-writer-group-id 2 --output-writer-id 2 \
  --delay-us 200 >"$TMP/hub.log" 2>&1 &
HUB_PID=$!

sleep 1
"$BIN_DIR/tsnhub_pubsub_fixture" publisher \
  --url "opc.udp://127.0.0.1:$INPUT_PORT" \
  --bind-url "opc.udp://0.0.0.0:$FIXTURE_BIND_PORT" \
  --publisher-id 1 --writer-group-id 1 --writer-id 1 --count 20 \
  >"$TMP/publisher.log" 2>&1
wait "$SUB_PID"
SUB_PID=""
kill -TERM "$HUB_PID"
wait "$HUB_PID"
HUB_PID=""

grep -q 'publisher final_value=20' "$TMP/publisher.log"
grep -q 'subscriber final_value=20' "$TMP/subscriber.log"
# hub 侧统计行: 收到的每一帧都必须被放行(无丢失、无队列积压), 且至少要覆盖
# 发布端写入的 20 个值。open62541 的 WriterGroup 在启动阶段还会补发一帧采样到的
# 变量初值, 该帧是否落在首次写入之前随时序而变, 因此 accepted/released 的确切
# 帧数在 20/21 之间波动, 这里只约束不变量而不绑定具体帧数。
hub_line=$(grep -o 'accepted=[0-9][0-9]* released=[0-9][0-9]* dropped_loss=[0-9][0-9]* dropped_queue=[0-9][0-9]*' "$TMP/hub.log" | tail -n 1)
if [ -z "$hub_line" ]; then
    echo "TsnHub hub stats line missing"
    exit 1
fi
rest=${hub_line#accepted=}
hub_accepted=${rest%% *}
rest=${rest#* }
hub_released=${rest%% *}
rest=${rest#* }
hub_dropped_loss=${rest%% *}
rest=${rest#* }
hub_dropped_queue=${rest%% *}
echo "hub $hub_line"
if [ "$hub_accepted" -lt 20 ] || [ "$hub_accepted" -ne "$hub_released" ] ||
   [ "$hub_dropped_loss" -ne 0 ] || [ "$hub_dropped_queue" -ne 0 ]; then
    echo "TsnHub hub forwarded frames incorrectly"
    exit 1
fi
echo "Native PubSub integration passed"
