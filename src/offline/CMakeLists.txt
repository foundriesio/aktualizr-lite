set(TARGET aklite_offline)

set(SRC
    fetcher.cc
    ostree_puller.cc
    apps_puller.cc
)
set(HEADERS
    fetcher.h
    ostree_puller.h
    apps_puller.h
)

add_library(${TARGET} STATIC ${SRC})

set(INCS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${AKTUALIZR_DIR}/include
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKLITE_DIR}/src/
  ${AKLITE_DIR}/include/
  ${LIBOSTREE_INCLUDE_DIRS}
  ${AKTUALIZR_DIR}/third_party/googletest/googletest/include
)

target_include_directories(${TARGET} PRIVATE ${INCS})
target_link_libraries(${TARGET} aktualizr_lib)

aktualizr_source_file_checks(${SRC} ${HEADERS})
