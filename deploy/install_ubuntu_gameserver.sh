#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "请用 sudo 运行此脚本。"
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y \
  build-essential \
  qt6-base-dev \
  qt6-base-dev-tools \
  libqt6sql6-mysql \
  libmysqlclient-dev \
  libtinyxml2-dev \
  rsync \
  pkg-config

mkdir -p /opt/jaeger-server
mkdir -p /opt/jaeger-server/logs
mkdir -p /opt/jaeger-server/data

if [[ ! -f /opt/jaeger-server/server.json ]] && [[ -f /opt/jaeger-server/server.json.example ]]; then
  cp /opt/jaeger-server/server.json.example /opt/jaeger-server/server.json
fi

echo "Ubuntu 依赖安装完成。"
echo "后续请把服务端源码或构建产物放到 /opt/jaeger-server 或你的发布目录。"
