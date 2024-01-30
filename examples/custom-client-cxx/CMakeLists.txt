cmake_minimum_required (VERSION 3.5)

set(TARGET custom-sota-client)
project(${TARGET})

set(SRC
  daemon.cpp
  cli.cpp
  main.cpp
)

add_executable(${TARGET} ${SRC})
target_link_libraries(${TARGET} boost_program_options)

target_compile_definitions(${TARGET} PRIVATE BOOST_LOG_DYN_LINK)
target_link_libraries(${TARGET} boost_log boost_filesystem pthread aktualizr_lite aktualizr)
install(TARGETS ${TARGET} RUNTIME DESTINATION bin)

