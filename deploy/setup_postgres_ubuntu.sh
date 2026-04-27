#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "请用 sudo 运行此脚本。"
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DB_NAME="${JAEGER_PG_DATABASE:-disk}"
DB_USER="${JAEGER_PG_USER:-jaeger_server}"
DB_PASSWORD="${JAEGER_PG_PASSWORD:-}"

if [[ -z "${DB_PASSWORD}" ]]; then
  echo "请先设置环境变量 JAEGER_PG_PASSWORD。"
  exit 1
fi

sudo -u postgres psql -v ON_ERROR_STOP=1 \
  --set=db_name="${DB_NAME}" \
  --set=db_user="${DB_USER}" \
  --set=db_password="${DB_PASSWORD}" <<'SQL'
SELECT format('CREATE ROLE %I LOGIN PASSWORD %L', :'db_user', :'db_password')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = :'db_user') \gexec

SELECT format('ALTER ROLE %I WITH LOGIN PASSWORD %L', :'db_user', :'db_password') \gexec

SELECT format('CREATE DATABASE %I OWNER %I', :'db_name', :'db_user')
WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = :'db_name') \gexec
SQL

sudo -u postgres psql -v ON_ERROR_STOP=1 -d "${DB_NAME}" -f "${SCRIPT_DIR}/init_postgres.sql"

echo "PostgreSQL 初始化完成。"
echo "数据库: ${DB_NAME}"
echo "用户: ${DB_USER}"
