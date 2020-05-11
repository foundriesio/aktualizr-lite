set(TEST_SOURCES test_lite.sh)

set(TEST_LIBS gtest gmock testutilities aktualizr_lib)

# TODO: Find a way to test without a need in the lite-mock
add_library(lite-mock SHARED ostree_mock.cc)
target_include_directories(lite-mock PRIVATE ${LIBOSTREE_INCLUDE_DIRS})

add_test(test_aktualizr-lite
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lite.sh
          ${CMAKE_BINARY_DIR}/src/aktualizr-lite
          ${CMAKE_BINARY_DIR}/aktualizr/src/uptane_generator/uptane-generator
          ${PROJECT_SOURCE_DIR}/aktualizr/tests
)

set_tests_properties(test_aktualizr-lite PROPERTIES DEPENDS uptane-generator)

add_aktualizr_test(NAME lite-helpers
  SOURCES helpers_test.cc ${AKLITE_DIR}/src/helpers.cc
  ARGS ${PROJECT_BINARY_DIR}/aktualizr/ostree_repo
)

target_compile_definitions(t_lite-helpers PRIVATE BOOST_LOG_DYN_LINK)
add_dependencies(t_lite-helpers lite-mock make_ostree_sysroot)

target_include_directories(t_lite-helpers PRIVATE
  ${AKLITE_DIR}/src
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  ${GLIB_INCLUDE_DIRS}
  ${LIBOSTREE_INCLUDE_DIRS}
)
target_link_libraries(t_lite-helpers aktualizr_lib ${Boost_LIBRARIES})

# TODO: Find a way to test without a need in the lite-mock
set_tests_properties(test_lite-helpers PROPERTIES ENVIRONMENT LD_PRELOAD=$<TARGET_FILE:lite-mock>)

if (BUILD_DOCKERAPP)
  target_compile_definitions(t_lite-helpers PRIVATE BUILD_DOCKERAPP)
endif(BUILD_DOCKERAPP)
