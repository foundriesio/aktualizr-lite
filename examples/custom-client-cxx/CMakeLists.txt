cmake_minimum_required (VERSION 3.5)
project(custom-sota-client)

add_executable(custom-sota-client main.cc)

target_compile_definitions(custom-sota-client PRIVATE BOOST_LOG_DYN_LINK)
target_link_libraries(custom-sota-client boost_log pthread aktualizr_lite aktualizr)
install(TARGETS custom-sota-client RUNTIME DESTINATION bin)
