# SPDX-License-Identifier: MIT

if(CONFIG_SHIELD_XIAO_ST7789_DIAG)
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/display_diag.c)
endif()
