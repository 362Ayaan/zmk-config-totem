cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(merry_display_controller)

# Seed slot A with the known-good Merry pack. Future pets are installed into
# the inactive raw slot and committed only after complete validation.
find_package(Python3 REQUIRED)
set(FACTORY_PET_IMAGE "${CMAKE_BINARY_DIR}/pet_a.bin")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../../tools/make_esp_pet_slot.py"
            "${CMAKE_CURRENT_LIST_DIR}/assets/merry-full.petpack"
            "${FACTORY_PET_IMAGE}" --pet-id merry --generation 1
    RESULT_VARIABLE FACTORY_PET_RESULT
)
if(NOT FACTORY_PET_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to prepare factory Merry pet slot")
endif()
esptool_py_flash_to_partition(flash pet_a "${FACTORY_PET_IMAGE}")
