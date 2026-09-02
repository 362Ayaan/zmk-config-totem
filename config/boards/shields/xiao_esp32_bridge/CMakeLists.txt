# SPDX-License-Identifier: MIT

if(CONFIG_SHIELD_XIAO_ESP32_BRIDGE)
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/merry_uart_status.c)
endif()
