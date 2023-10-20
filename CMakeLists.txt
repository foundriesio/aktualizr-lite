# Copyright (c) 2020 Foundries.io
#
# SPDX-License-Identifier: MIT

cmake_minimum_required (VERSION 3.5)
project(aklite)

set(AKTUALIZR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/aktualizr)
set(AKLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MAIN_TARGET_LIB aktualizr_lite)
set(AKLITE_OFFLINE_LIB aklite-offline_lib)

# export compile commands to make teh aktualizr's clang-tidy stuff working
# see aktualizr/CMakeLists.txt for more details
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OSTree is mandatory for aktualizr-lite
option(BUILD_OSTREE "Set to ON to compile with ostree support" ON)

option(ALLOW_MANUAL_ROLLBACK "Set to ON to build with support of manual rollbacks" OFF)
option(BUILD_AKLITE_OFFLINE "Set to ON to build cli command for an offline update" OFF)
option(BUILD_AKLITE_WITH_NERDCTL "Set to ON to build with support of nerdctl/containerd" OFF)
option(BUILD_AKLITE_APPS "Set to ON to build cli command for Apps management" ON)
option(BUILD_TUFCTL "Set to ON to build sample tuf control application" OFF)

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
  add_dependencies(aklite-tests aklite t_lite-helpers uptane-generator t_compose-apps t_ostree t_liteclient t_yaml2json t_composeappengine t_restorableappengine t_aklite t_aklite_rollback t_aklite_rollback_ext t_apiclient t_exec t_docker t_aklite_offline t_boot_flag_mgmt t_cli t_nospace)

  set(CMAKE_MODULE_PATH "${AKTUALIZR_DIR}/cmake-modules;${CMAKE_MODULE_PATH}")

  find_package(OSTree REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_search_module(GLIB REQUIRED glib-2.0)

  add_subdirectory(src)

  include(CTest)
  add_subdirectory(tests EXCLUDE_FROM_ALL)

  if(BUILD_AKLITE_APPS)
    add_subdirectory(apps/aklite-apps)
  endif(BUILD_AKLITE_APPS)
  if(BUILD_AKLITE_OFFLINE)
    add_subdirectory(apps/aklite-offline)
  endif(BUILD_AKLITE_OFFLINE)
  if(BUILD_TUFCTL)
    add_subdirectory(apps/tufctl)
  endif(BUILD_TUFCTL)
endif(BUILD_AKLITE)

# Use `-LH` options (cmake <args> -LH) to output all variables
message(STATUS "BUILD_AKLITE: ${BUILD_AKLITE}")
message(STATUS "BUILD_OSTREE: ${BUILD_OSTREE}")
message(STATUS "BUILD_SOTA_TOOLS: ${BUILD_SOTA_TOOLS}")
message(STATUS "ALLOW_MANUAL_ROLLBACK: ${ALLOW_MANUAL_ROLLBACK}")
message(STATUS "BUILD_AKLITE_OFFLINE: ${BUILD_AKLITE_OFFLINE}")
message(STATUS "BUILD_AKLITE_WITH_NERDCTL: ${BUILD_AKLITE_WITH_NERDCTL}")
message(STATUS "BUILD_AKLITE_APPS: ${BUILD_AKLITE_APPS}")
message(STATUS "BUILD_TUFCTL: ${BUILD_TUFCTL}")
