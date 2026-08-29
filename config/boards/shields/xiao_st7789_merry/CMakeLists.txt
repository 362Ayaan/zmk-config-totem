# SPDX-License-Identifier: MIT

if(CONFIG_SHIELD_XIAO_ST7789_MERRY)
  target_sources(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/custom_status_screen.c
    ${CMAKE_CURRENT_LIST_DIR}/pet_store.c
    ${CMAKE_CURRENT_LIST_DIR}/pet_upload.c
    ${CMAKE_CURRENT_LIST_DIR}/generated/merry_fallback.c
  )
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/generated)
endif()
