#!/bin/sh
set -eu

BIN_DIR=${1:-.}
BASE_PORT=${TSNHUB_TEST_BASE_PORT:-4851}
INPUT_PORT=$BASE_PORT
OUTPUT_PORT=$((BASE_PORT + 1))
TMP=${TMPDIR:-/tmp}/tsnhub-native-$$
mkdir -p "$TMP"
HUB_PID=""
SUB_PID=""
cleanup() {
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
  --input-publisher-id 1 --input-writer-group-id 1 --input-writer-id 1 \
  --output-publisher-id 2 --output-writer-group-id 2 --output-writer-id 2 \
  --delay-us 200 >"$TMP/hub.log" 2>&1 &
HUB_PID=$!

sleep 1
"$BIN_DIR/tsnhub_pubsub_fixture" publisher \
  --url "opc.udp://127.0.0.1:$INPUT_PORT" \
  --publisher-id 1 --writer-group-id 1 --writer-id 1 --count 20 \
  >"$TMP/publisher.log" 2>&1
wait "$SUB_PID"
SUB_PID=""
kill -TERM "$HUB_PID"
wait "$HUB_PID"
HUB_PID=""

grep -q 'publisher final_value=20' "$TMP/publisher.log"
grep -q 'subscriber final_value=20' "$TMP/subscriber.log"
grep -q 'accepted=20 released=20' "$TMP/hub.log"
echo "Native PubSub integration passed"
