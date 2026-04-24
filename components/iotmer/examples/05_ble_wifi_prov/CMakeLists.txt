cmake_minimum_required(VERSION 3.16)

# Parent of `iotmer/`: the repo’s `components/` in the monorepo, or a project’s
# `managed_components/` when this tree lives under `…/iotmertech__iotmer/examples/…`.
# That lets the resolver find `iotmer` and `iotmer_ble_wifi_prov` before `main`’s REQUIRES.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(iotmer_05_ble_wifi_prov)
