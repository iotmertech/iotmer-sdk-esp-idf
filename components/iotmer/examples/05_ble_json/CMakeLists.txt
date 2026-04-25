cmake_minimum_required(VERSION 3.16)

# This example lives under the component tree so the ESP Component Registry can index it.
# For local builds in this monorepo, point the project at the repository `components/` folder.
set(EXTRA_COMPONENT_DIRS
    "../../.."
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(iotmer_05_ble_json)

