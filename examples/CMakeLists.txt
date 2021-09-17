cmake_minimum_required (VERSION 3.5)

add_executable(custom-client custom-client.cc)
target_compile_definitions(custom-client PRIVATE BOOST_LOG_DYN_LINK)
target_include_directories(custom-client PRIVATE ${AKLITE_DIR}/include ${AKTUALIZR_DIR}/third_party/jsoncpp/include ${AKTUALIZR_DIR}/src/libaktualizr/logging)
target_link_libraries(custom-client boost_log pthread aktualizr_lib aktualizr_lite)
install(TARGETS custom-client RUNTIME DESTINATION bin)
aktualizr_source_file_checks(custom-client.cc)
