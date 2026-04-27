cmake_minimum_required(VERSION 3.16)

# `components/iotmer` bu örnek `components/iotmer/examples/01_provisioning` altında; üç üst dizin repo `components` kökü.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(iotmer_01_provisioning)
