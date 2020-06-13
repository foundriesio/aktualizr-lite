set(TEST_SOURCES test_lite.sh)

# TODO: add more gtests
# TODO: add static checker
# TODO: add valgrind
add_test(test_aktualizr-lite
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lite.sh
          ${CMAKE_BINARY_DIR}/src/aktualizr-lite
          ${CMAKE_BINARY_DIR}/aktualizr/src/uptane_generator/uptane-generator
          ${PROJECT_SOURCE_DIR}/aktualizr/tests
          ${CMAKE_CURRENT_SOURCE_DIR}/compose_fake.sh
)

set_tests_properties(test_aktualizr-lite PROPERTIES DEPENDS uptane-generator)

set(TEST_DEFS BOOST_LOG_DYN_LINK)
set(TEST_INCS
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  ${GLIB_INCLUDE_DIRS}
  ${LIBOSTREE_INCLUDE_DIRS}
  ${AKLITE_DIR}/src
)
set(TEST_LIBS
  aktualizr_lib
  ${Boost_LIBRARIES}
  gtest
)

add_aktualizr_test(NAME compose-apps
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> composeapp_test.cc
  PROJECT_WORKING_DIRECTORY ARGS ${PROJECT_BINARY_DIR}/aktualizr/ostree_repo
)

target_compile_definitions(t_compose-apps PRIVATE ${TEST_DEFS})
target_include_directories(t_compose-apps PRIVATE ${AKTUALIZR_DIR}/tests ${TEST_INCS})
target_link_libraries(t_compose-apps ${TEST_LIBS})

add_aktualizr_test(NAME lite-helpers
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> helpers_test.cc
  PROJECT_WORKING_DIRECTORY ARGS ${PROJECT_BINARY_DIR}/aktualizr/ostree_repo
)

target_compile_definitions(t_lite-helpers PRIVATE ${TEST_DEFS})
target_include_directories(t_lite-helpers PRIVATE ${AKTUALIZR_DIR}/tests ${TEST_INCS})
target_link_libraries(t_lite-helpers ${TEST_LIBS})

add_dependencies(t_lite-helpers make_ostree_sysroot)
