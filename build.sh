#!/usr/bin/env bash
set -euo pipefail

# Always run from repository root.
cd "$(dirname "$0")"

BOARD_TYPE="m5stack-tab5"
BOARD_NAME="m5stack-tab5"
TARGET="esp32p4"

SDKCONFIG_APPEND=(
  "CONFIG_BOARD_TYPE_M5STACK_CORE_TAB5=y"
  "CONFIG_CAMERA_SC202CS=y"
  "CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE=y"
  "CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=13"
  "CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1=12"
  "CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1=11"
  "CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1=10"
  "CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1=9"
  "CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1=8"
)

PROJECT_VER="$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt | head -n1)"
if [[ -z "${PROJECT_VER}" ]]; then
  echo "failed to parse PROJECT_VER from CMakeLists.txt" >&2
  exit 1
fi

unset IDF_TARGET

idf.py set-target "${TARGET}"

{
  echo
  echo "# Append by build.sh"
  for entry in "${SDKCONFIG_APPEND[@]}"; do
    echo "${entry}"
  done
} >> sdkconfig

idf.py -DBOARD_NAME="${BOARD_NAME}" -DBOARD_TYPE="${BOARD_TYPE}" build
idf.py merge-bin

mkdir -p releases
python -m zipfile -c "releases/v${PROJECT_VER}_${BOARD_NAME}.zip" build/merged-binary.bin
echo "Built: releases/v${PROJECT_VER}_${BOARD_NAME}.zip"
