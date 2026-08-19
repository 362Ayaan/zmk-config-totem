# SPDX-License-Identifier: MIT

if(CONFIG_SHIELD_XIAO_SSD1306)
  target_sources(app PRIVATE custom_status_screen.c)
endif()
