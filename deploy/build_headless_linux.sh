#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${SERVER_DIR}/build/linux-headless-release}"

if command -v qmake6 >/dev/null 2>&1; then
  QMAKE_BIN="$(command -v qmake6)"
elif command -v qmake >/dev/null 2>&1; then
  QMAKE_BIN="$(command -v qmake)"
else
  echo "未找到 qmake6 或 qmake，请先安装 Qt 构建工具。"
  exit 1
fi

if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
else
  JOBS="4"
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

"${QMAKE_BIN}" "${SERVER_DIR}/GameServerHeadless.pro" CONFIG+=release
make -j"${JOBS}"

echo "Headless 服务端构建完成：${BUILD_DIR}/GameServer"
