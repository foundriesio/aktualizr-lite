set(TEST_SOURCES test_lite.sh)

# TODO: Find a way to test without a need in the lite-mock
add_library(lite-mock SHARED ostree_mock.cc)
target_include_directories(lite-mock PRIVATE ${LIBOSTREE_INCLUDE_DIRS})

# TODO: add more gtests
# TODO: add static checker
# TODO: add valgrind
add_test(test_aktualizr-lite
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lite.sh
          ${CMAKE_BINARY_DIR}/src/aktualizr-lite
          ${CMAKE_BINARY_DIR}/aktualizr/src/uptane_generator/uptane-generator
          ${PROJECT_SOURCE_DIR}/aktualizr/tests
)

set_tests_properties(test_aktualizr-lite PROPERTIES DEPENDS uptane-generator)

set(TEST_DEFS BOOST_LOG_DYN_LINK)
set(TEST_INCS
  ${AKLITE_DIR}/src
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  ${GLIB_INCLUDE_DIRS}
  ${LIBOSTREE_INCLUDE_DIRS}
)
set(TEST_LIBS
  aktualizr_lib
  ${Boost_LIBRARIES}
  gtest
)

add_aktualizr_test(NAME compose-apps
  SOURCES ${AKLITE_DIR}/src/composeappmanager.cc composeapp_test.cc
  PROJECT_WORKING_DIRECTORY ARGS ${PROJECT_BINARY_DIR}/aktualizr/ostree_repo
)

target_compile_definitions(t_compose-apps PRIVATE ${TEST_DEFS})
target_include_directories(t_compose-apps PRIVATE ${AKTUALIZR_DIR}/tests ${TEST_INCS})
target_link_libraries(t_compose-apps ${TEST_LIBS})

add_aktualizr_test(NAME lite-helpers
  SOURCES helpers_test.cc ${AKLITE_DIR}/src/helpers.cc ${AKLITE_DIR}/src/composeappmanager.cc
  ARGS ${PROJECT_BINARY_DIR}/aktualizr/ostree_repo
)

target_compile_definitions(t_lite-helpers PRIVATE ${TEST_DEFS})
target_include_directories(t_lite-helpers PRIVATE ${AKTUALIZR_DIR}/tests ${TEST_INCS})
target_link_libraries(t_lite-helpers ${TEST_LIBS})

add_dependencies(t_lite-helpers lite-mock make_ostree_sysroot)

# TODO: Find a way to test without a need in the lite-mock
set_tests_properties(test_lite-helpers PROPERTIES ENVIRONMENT LD_PRELOAD=$<TARGET_FILE:lite-mock>)

if (BUILD_DOCKERAPP)
  target_compile_definitions(t_lite-helpers PRIVATE BUILD_DOCKERAPP)
endif(BUILD_DOCKERAPP)
