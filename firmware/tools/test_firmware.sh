#!/usr/bin/env bash
# 主机端固件纯函数测试（不需要 Arduino/硬件）。
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-clang++}
$CXX -std=c++17 -Wall -Wextra -O2 \
  -o /tmp/test_ui_theme \
  tests/test_ui_theme.cpp habit_coach/ui_theme.cpp
/tmp/test_ui_theme

$CXX -std=c++17 -Wall -Wextra -O2 \
  -o /tmp/test_habit_sync \
  tests/test_habit_sync.cpp
/tmp/test_habit_sync
