# Copyright (c) 2020 Foundries.io
#
# SPDX-License-Identifier: MIT

cmake_minimum_required (VERSION 3.5)
project(aklite)

set(AKTUALIZR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/aktualizr)
set(AKLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MAIN_TARGET_LIB aktualizr-lite_lib)

# export compile commands to make teh aktualizr's clang-tidy stuff working
# see aktualizr/CMakeLists.txt for more details
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OSTree is mandatory for aktualizr-lite
option(BUILD_OSTREE "Set to ON to compile with ostree support" ON)

option(ALLOW_MANUAL_ROLLBACK "Set to ON to build with support of manual rollbacks" OFF)

# If we build the sota tools we don't need aklite (???) and vice versa
# if we build aklite we don't need the sota tools
# TODO: consider using the aktualizr repo/project for building the sota-tools
# and using aktualizr-repo solely for building aklite
if(BUILD_SOTA_TOOLS)
  set(BUILD_AKLITE OFF)
else(BUILD_SOTA_TOOLS)
  set(BUILD_AKLITE ON)
endif(BUILD_SOTA_TOOLS)

# TODO: consider adding a custom target or using ExternalProject so we don't need
# to encapsulate/inherit all aktualizr's targets and definitions, so e.g. `make all`
# would aktually means build 'all' of aktualizr-lite but not 'all' of aktualizr
add_subdirectory(${AKTUALIZR_DIR})

if(BUILD_AKLITE)
  add_custom_target(aklite)
  add_dependencies(aklite aktualizr-lite)

  add_custom_target(aklite-tests)
  add_dependencies(aklite-tests aklite t_lite-helpers uptane-generator t_compose-apps t_ostree t_liteclient t_yaml2json t_composeappengine)

  set(CMAKE_MODULE_PATH "${AKTUALIZR_DIR}/cmake-modules;${CMAKE_MODULE_PATH}")

  find_package(OSTree REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_search_module(GLIB REQUIRED glib-2.0)

  add_subdirectory(src)

  include(CTest)
  add_subdirectory(tests EXCLUDE_FROM_ALL)

endif(BUILD_AKLITE)

# Use `-LH` options (cmake <args> -LH) to output all variables
message(STATUS "BUILD_AKLITE: ${BUILD_AKLITE}")
message(STATUS "BUILD_OSTREE: ${BUILD_OSTREE}")
message(STATUS "BUILD_SOTA_TOOLS: ${BUILD_SOTA_TOOLS}")
message(STATUS "ALLOW_MANUAL_ROLLBACK: ${ALLOW_MANUAL_ROLLBACK}")
