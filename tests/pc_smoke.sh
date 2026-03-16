#!/usr/bin/env bash
set -euo pipefail
trap '' PIPE

BIN_PATH="${1:-./build/p2p-stream}"
PROFILES_DIR="${2:-./device-profiles}"
TEST_ROOT="${3:-./build/test-artifacts/pc-smoke}"
RUN_DIR="${TEST_ROOT}/run"
LOG_FILE="${TEST_ROOT}/app.log"
STDOUT_FILE="${TEST_ROOT}/stdout.log"

mkdir -p "${RUN_DIR}"
mkdir -p "${TEST_ROOT}"
rm -f "${RUN_DIR}/p2p-stream.cmd" "${RUN_DIR}/p2p-stream.status" "${LOG_FILE}" "${STDOUT_FILE}"

if [[ ! -x "${BIN_PATH}" ]]; then
    echo "[FAIL] Binary not found or not executable: ${BIN_PATH}" >&2
    exit 1
fi

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[FAIL] Required command missing: $1" >&2
        exit 1
    fi
}

need_gst_plugin() {
    local plugin="$1"
    if ! gst-inspect-1.0 "${plugin}" >/dev/null 2>&1; then
        echo "[FAIL] Missing GStreamer plugin: ${plugin}" >&2
        exit 1
    fi
}

wait_for_file() {
    local path="$1"
    local timeout_s="$2"
    local i
    for ((i=0; i<timeout_s*10; i++)); do
        if [[ -e "${path}" ]]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

assert_status_contains() {
    local text="$1"
    local retries=20
    local i
    for ((i=0; i<retries; i++)); do
        if [[ -f "${RUN_DIR}/p2p-stream.status" ]] && grep -q "${text}" "${RUN_DIR}/p2p-stream.status"; then
            return 0
        fi
        sleep 0.2
    done
    echo "[FAIL] Status file does not contain '${text}'" >&2
    if [[ -f "${RUN_DIR}/p2p-stream.status" ]]; then
        echo "--- status file ---" >&2
        cat "${RUN_DIR}/p2p-stream.status" >&2
    fi
    return 1
}

send_cmd() {
    local cmd="$1"
    local i
    for i in $(seq 1 40); do
        if printf "%s\n" "${cmd}" > "${RUN_DIR}/p2p-stream.cmd" 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    echo "[FAIL] Could not write command '${cmd}' to FIFO" >&2
    return 1
}

cleanup() {
    if [[ -n "${APP_PID:-}" ]] && kill -0 "${APP_PID}" >/dev/null 2>&1; then
        kill -TERM "${APP_PID}" >/dev/null 2>&1 || true
        wait "${APP_PID}" || true
    fi
}
trap cleanup EXIT

need_cmd gst-inspect-1.0
need_gst_plugin videotestsrc
need_gst_plugin x264enc
need_gst_plugin h264parse
need_gst_plugin rtph264pay
need_gst_plugin udpsink

# Negative CLI validation tests (must fail).
if "${BIN_PATH}" --net-role ap --role sender --device bad/name --profiles "${PROFILES_DIR}" >/dev/null 2>&1; then
    echo "[FAIL] Invalid device name should fail" >&2
    exit 1
fi

if "${BIN_PATH}" --net-role ap --role sender --device pc --width abc --profiles "${PROFILES_DIR}" >/dev/null 2>&1; then
    echo "[FAIL] Invalid width should fail" >&2
    exit 1
fi

export P2P_STREAM_RUN_DIR="${RUN_DIR}"

"${BIN_PATH}" \
    --net-role ap \
    --role sender \
    --device pc \
    --codec h264 \
    --trigger manual \
    --profiles "${PROFILES_DIR}" \
    --peer-ip 127.0.0.1 \
    --test-pattern \
    --width 320 \
    --height 240 \
    --fps 15 \
    --bitrate 500000 \
    --log-file "${LOG_FILE}" \
    >"${STDOUT_FILE}" 2>&1 &
APP_PID=$!

if ! wait_for_file "${RUN_DIR}/p2p-stream.cmd" 10; then
    echo "[FAIL] Control FIFO was not created" >&2
    exit 1
fi

if ! wait_for_file "${RUN_DIR}/p2p-stream.status" 10; then
    echo "[FAIL] Status file was not created" >&2
    exit 1
fi

# Basic lifecycle
send_cmd "start"
assert_status_contains "STREAMING"

send_cmd "stop"
assert_status_contains "IDLE"

# Command storm to exercise queue + dedupe logic
for _ in $(seq 1 15); do
    send_cmd "start"
    send_cmd "stop"
done

if ! kill -0 "${APP_PID}" >/dev/null 2>&1; then
    echo "[FAIL] App exited unexpectedly during command storm" >&2
    exit 1
fi

kill -TERM "${APP_PID}" >/dev/null 2>&1 || true
wait "${APP_PID}"
APP_PID=""

echo "[PASS] pc_smoke"
