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
set_tests_properties(test_aktualizr-lite PROPERTIES LABELS "aklite:aktualizr-lite")

set(TEST_DEFS BOOST_LOG_DYN_LINK)
set(TEST_INCS
  ${AKLITE_DIR}/src/compose
  ${AKLITE_DIR}/src/
  ${AKTUALIZR_DIR}/include
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  ${GLIB_INCLUDE_DIRS}
  ${LIBOSTREE_INCLUDE_DIRS}non_init_repo_dir
  ${AKLITE_DIR}/src
)
set(TEST_LIBS
  aktualizr_lib
  ${Boost_LIBRARIES}
  gtest
	gmock
)

add_aktualizr_test(NAME yaml2json
	SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> yaml2json_test.cc
	PROJECT_WORKING_DIRECTORY
)

target_compile_definitions(t_yaml2json PRIVATE ${TEST_DEFS})
target_include_directories(t_yaml2json PRIVATE ${AKTUALIZR_DIR}/tests ${TEST_INCS})
target_link_libraries(t_yaml2json ${TEST_LIBS})
set_tests_properties(test_yaml2json PROPERTIES LABELS "aklite:yaml2json")

aktualizr_source_file_checks(yaml2json_test.cc)

add_aktualizr_test(NAME compose-apps
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> composeapp_test.cc
  PROJECT_WORKING_DIRECTORY ARGS ${PROJECT_BINARY_DIR}/aktualizr/ostree_repo
)

target_compile_definitions(t_compose-apps PRIVATE ${TEST_DEFS})
target_include_directories(t_compose-apps PRIVATE ${AKTUALIZR_DIR}/tests ${TEST_INCS})
target_link_libraries(t_compose-apps ${TEST_LIBS})
set_tests_properties(test_compose-apps PROPERTIES LABELS "aklite:compose-apps")

add_aktualizr_test(NAME lite-helpers
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> helpers_test.cc
  PROJECT_WORKING_DIRECTORY ARGS ${PROJECT_BINARY_DIR}/aktualizr/ostree_repo
)

target_compile_definitions(t_lite-helpers PRIVATE ${TEST_DEFS})
target_include_directories(t_lite-helpers PRIVATE ${AKTUALIZR_DIR}/tests ${TEST_INCS})
target_link_libraries(t_lite-helpers ${TEST_LIBS})
add_dependencies(t_lite-helpers make_ostree_sysroot)
set_tests_properties(test_lite-helpers PROPERTIES LABELS "aklite:helpers")

add_aktualizr_test(NAME ostree
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> ostree_test.cc
  PROJECT_WORKING_DIRECTORY
)

target_compile_definitions(t_ostree PRIVATE ${TEST_DEFS})
target_include_directories(t_ostree PRIVATE ${TEST_INCS})
target_link_libraries(t_ostree ${TEST_LIBS})
set_tests_properties(test_ostree PROPERTIES LABELS "aklite:ostree")

add_aktualizr_test(NAME liteclient
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> liteclient_test.cc
  PROJECT_WORKING_DIRECTORY
  ARGS
  ${PROJECT_SOURCE_DIR}/tests/device-gateway_fake.py
  ${PROJECT_SOURCE_DIR}/tests/make_sys_rootfs.sh
)

target_compile_definitions(t_liteclient PRIVATE ${TEST_DEFS})
target_include_directories(t_liteclient PRIVATE ${TEST_INCS} ${AKTUALIZR_DIR}/tests/ ${AKTUALIZR_DIR}/src/)
target_link_libraries(t_liteclient ${TEST_LIBS} uptane_generator_lib testutilities)
add_dependencies(t_liteclient make_ostree_sysroot)
set_tests_properties(test_liteclient PROPERTIES LABELS "aklite:liteclient")

add_aktualizr_test(NAME liteclientHSM
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> liteclientHSM_test.cc
  PROJECT_WORKING_DIRECTORY
  ARGS
  ${PROJECT_SOURCE_DIR}/tests/device-gateway_fake.py
  ${PROJECT_SOURCE_DIR}/tests/make_sys_rootfs.sh
)

target_compile_definitions(t_liteclientHSM PRIVATE ${TEST_DEFS})
target_include_directories(t_liteclientHSM PRIVATE ${TEST_INCS} ${AKTUALIZR_DIR}/tests/ ${AKTUALIZR_DIR}/src/)
target_link_libraries(t_liteclientHSM ${TEST_LIBS} uptane_generator_lib testutilities)
add_dependencies(t_liteclientHSM make_ostree_sysroot)
set_tests_properties(test_liteclientHSM PROPERTIES LABELS "aklite:liteclientHSM")

aktualizr_source_file_checks(liteclient_test.cc)
aktualizr_source_file_checks(liteclientHSM_test.cc)

add_aktualizr_test(NAME composeappengine
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> composeappengine_test.cc
  PROJECT_WORKING_DIRECTORY
)

target_compile_definitions(t_composeappengine PRIVATE ${TEST_DEFS})
target_include_directories(t_composeappengine PRIVATE ${TEST_INCS} ${AKTUALIZR_DIR}/tests/ ${AKTUALIZR_DIR}/src/)
set_tests_properties(test_composeappengine PROPERTIES LABELS "aklite:appengine")

aktualizr_source_file_checks(composeappengine_test.cc)


add_aktualizr_test(NAME aklite
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> aklite_test.cc
  PROJECT_WORKING_DIRECTORY
  ARGS
  ${PROJECT_SOURCE_DIR}/tests/device-gateway_fake.py
  ${PROJECT_SOURCE_DIR}/tests/make_sys_rootfs.sh
)

target_compile_definitions(t_aklite PRIVATE ${TEST_DEFS})
target_include_directories(t_aklite PRIVATE ${TEST_INCS} ${AKTUALIZR_DIR}/tests/ ${AKTUALIZR_DIR}/src/)
target_link_libraries(t_aklite ${TEST_LIBS} uptane_generator_lib testutilities)
add_dependencies(t_aklite make_ostree_sysroot)
set_tests_properties(test_aklite PROPERTIES LABELS "aklite:aklite")

aktualizr_source_file_checks(aklite_test.cc)

add_aktualizr_test(NAME apiclient
  SOURCES $<TARGET_OBJECTS:${MAIN_TARGET_LIB}> apiclient_test.cc
  PROJECT_WORKING_DIRECTORY
  ARGS
  ${PROJECT_SOURCE_DIR}/tests/device-gateway_fake.py
  ${PROJECT_SOURCE_DIR}/tests/make_sys_rootfs.sh
)
aktualizr_source_file_checks(apiclient_test.cc)
target_compile_definitions(t_apiclient PRIVATE ${TEST_DEFS})
target_include_directories(t_apiclient PRIVATE ${TEST_INCS} ${AKLITE_DIR}/include ${AKTUALIZR_DIR}/tests/ ${AKTUALIZR_DIR}/src/)
target_link_libraries(t_apiclient aktualizr_lite ${TEST_LIBS} uptane_generator_lib testutilities)
add_dependencies(t_apiclient make_ostree_sysroot)
set_tests_properties(test_apiclient PROPERTIES LABELS "aklite:apiclient")

