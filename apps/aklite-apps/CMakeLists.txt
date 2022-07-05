cmake_minimum_required (VERSION 3.5)

set(TARGET aklite-apps)
project(${TARGET})

set(SRC main.cpp cmds.cpp)
set(HEADERS cmds.h)

add_executable(${TARGET} ${SRC})
add_dependencies(aklite-tests ${TARGET})

set(INCS
  ${AKLITE_DIR}/include/
  ${AKLITE_DIR}/src/
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/include
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  # odd dependency of libaktualizr/http/httpclient.h
  ${AKTUALIZR_DIR}/third_party/googletest/googletest/include
)

target_include_directories(${TARGET} PRIVATE ${INCS})

target_link_libraries(${TARGET} aktualizr_lite)

install(TARGETS ${TARGET} RUNTIME DESTINATION bin)

# enable creating clang-tidy targets for each source file (see aktualizr/CMakeLists.txt for details)
aktualizr_source_file_checks(${SRC} ${HEADERS})
