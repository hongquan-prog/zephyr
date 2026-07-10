# Copyright (c) 2023 Antmicro <www.antmicro.com>
# SPDX-License-Identifier: Apache-2.0

board_runner_args(jlink "--device=AMAP42KK-KBR" "--iface=swd" "--speed=1000")

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)

if(CONFIG_BOARD_APOLLO4P_EVB)
  set(SUPPORTED_EMU_PLATFORMS qemu)
  set(QEMU_CPU_TYPE_${ARCH} cortex-m4)

  if(NOT DEFINED AMBIQ_APOLLO4P_EVB_QEMU_SD_IMG)
    set(AMBIQ_APOLLO4P_EVB_QEMU_SD_IMG ${ZEPHYR_BASE}/../sd.img)
  endif()

  set(QEMU_FLAGS_${ARCH}
    -machine ambiq-apollo4p-evb
    -drive file=${AMBIQ_APOLLO4P_EVB_QEMU_SD_IMG},format=raw,if=sd,index=0
  )

  include(${ZEPHYR_BASE}/boards/common/qemu.board.cmake)
endif()
