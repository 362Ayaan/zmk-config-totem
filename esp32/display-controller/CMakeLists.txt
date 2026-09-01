cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(merry_display_controller)

# Include the complete 30-frame RGB565 pet pack in the merged flash artifact.
spiffs_create_partition_image(assets assets FLASH_IN_PROJECT)
