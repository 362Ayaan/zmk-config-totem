# SPDX-License-Identifier: MIT

if(CONFIG_SHIELD_XIAO_ST7789_MERRY)
  target_sources(app PRIVATE
    custom_status_screen.c
    pet_store.c
    pet_upload.c
    generated/merry_fallback.c
  )
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated)
endif()
