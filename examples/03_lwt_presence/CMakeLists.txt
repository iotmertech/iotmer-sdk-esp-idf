cmake_minimum_required(VERSION 3.16)

# Pick up `components/iotmer` from the repository root (sibling of `examples/`).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(iotmer_03_lwt_presence)

