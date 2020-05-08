# Copyright (c) 2020 Foundries.io
#
# SPDX-License-Identifier: MIT

cmake_minimum_required (VERSION 3.5)
project(aklite)

# OSTree and DockerApp package manager are mandatory for aktualizr-lite
set(BUILD_OSTREE ON CACHE BOOL "")
set(BUILD_DOCKERAPP ON CACHE BOOL "")

add_custom_target(aklite)
add_dependencies(aklite aktualizr-lite)

add_custom_target(aklite-tests)
add_dependencies(aklite-tests aklite t_lite-helpers uptane-generator)

# Export compile_commands.json for clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/aktualizr/cmake-modules;${CMAKE_MODULE_PATH}")

if(NOT BUILD_SOTA_TOOLS)
  find_package(OSTree REQUIRED)
  include_directories(${GLIB_INCLUDE_DIRS} ${LIBOSTREE_INCLUDE_DIRS})
endif(NOT BUILD_SOTA_TOOLS)

include(CTest)

set(AKTUALIZR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/aktualizr)
set(AKLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# TODO: consider adding a custom target or using ExternalProject so we don't need
# to encapsulate/inherit all aktualizr's targets and definitions, so e.g. `make all`
# would aktually means build 'all' of aktualizr-lite but not 'all' of altualizr
add_subdirectory(${AKTUALIZR_DIR})


if(NOT BUILD_SOTA_TOOLS)
    add_subdirectory(src)
    add_subdirectory(tests EXCLUDE_FROM_ALL)
endif(NOT BUILD_SOTA_TOOLS)

# TODO: consider cleaning up the "install" target elements as it includes
# redundant targets (e.g aktualizr-secondary, aktualizr-cert-provider, etc)
install(TARGETS ${TARGET} RUNTIME DESTINATION bin COMPONENT ${TARGET})
