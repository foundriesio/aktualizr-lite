# Copyright (c) 2020 Foundries.io
#
# SPDX-License-Identifier: MIT

cmake_minimum_required (VERSION 3.5)

# Export compile_commands.json for clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_definitions(-DBOOST_LOG_DYN_LINK)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/aktualizr/src/libaktualizr)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/aktualizr/third_party/jsoncpp/include)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/aktualizr/cmake-modules;${CMAKE_MODULE_PATH}")

find_package(OSTree REQUIRED)
include_directories(${GLIB_INCLUDE_DIRS} ${LIBOSTREE_INCLUDE_DIRS})

include(CTest)

add_subdirectory("aktualizr")
add_subdirectory("src")
