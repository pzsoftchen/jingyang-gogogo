#!/usr/bin/env bash
# 清空演示数据，准备正式给孩子用
set -e
cd "$(dirname "$0")/.."
pkill -f "server.py" 2>/dev/null || true
sleep 0.6
rm -f coach.db coach.db-shm coach.db-wal
echo "已清空 coach.db。重新启动：python3 server.py"
