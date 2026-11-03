#!/usr/bin/env bash
# 编译两块板（不需要硬件）。用法：./tools/build.sh [devkitc|185|all]
set -euo pipefail
cd "$(dirname "$0")/.."

SKETCH=firmware/habit_coach
# ESP32-S3：16MB Flash + 八线(OPI) PSRAM + Huge APP 分区
FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app"

build() {
  local label="$1" macro="$2"
  echo "======== 编译 $label ========"
  arduino-cli compile \
    --fqbn "$FQBN" \
    --build-property "compiler.cpp.extra_flags=-DBOARD=$macro" \
    --warnings default \
    "$SKETCH"
}

case "${1:-all}" in
  devkitc) build "DevKitC + GC9A01 (240)" BOARD_DEVKITC_GC9A01 ;;
  185)     build "Waveshare 1.85 裸板 (360)"    BOARD_WS_1_85 ;;
  185c)    build "Waveshare 1.85C 带外壳 (360)" BOARD_WS_1_85C ;;
  all)     build "DevKitC + GC9A01 (240)"      BOARD_DEVKITC_GC9A01
           build "Waveshare 1.85 裸板 (360)"    BOARD_WS_1_85
           build "Waveshare 1.85C 带外壳 (360)" BOARD_WS_1_85C ;;
  *) echo "用法: $0 [devkitc|185|185c|all]"; exit 1 ;;
esac
