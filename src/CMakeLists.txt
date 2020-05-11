set(TARGET aktualizr-lite)
set(SRC main.cc helpers.cc)
set(HEADERS helpers.h)

add_executable(${TARGET} ${SRC})

target_include_directories(${TARGET} PRIVATE ${AKTUALIZR_DIR}/src/libaktualizr)
target_include_directories(${TARGET} PRIVATE ${AKTUALIZR_DIR}/third_party/jsoncpp/include)

target_link_libraries(${TARGET} aktualizr_lib)

if (BUILD_DOCKERAPP)
  target_compile_definitions(${TARGET} PRIVATE BUILD_DOCKERAPP)
endif(BUILD_DOCKERAPP)
