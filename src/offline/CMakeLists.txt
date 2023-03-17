set(TARGET ${AKLITE_OFFLINE_LIB})

set(SRC client.cc)
set(HEADERS client.h)

add_library(${TARGET} OBJECT ${SRC})
set_property(TARGET ${TARGET} PROPERTY CXX_STANDARD 17)

target_include_directories(${TARGET} PRIVATE
  ${AKLITE_DIR}/include/
  ${AKTUALIZR_DIR}/include
  ${AKLITE_DIR}/src
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/third_party/googletest/googletest/include
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  ${LIBOSTREE_INCLUDE_DIRS}
)

target_link_libraries(${TARGET} aktualizr_lib)

aktualizr_source_file_checks(${SRC} ${HEADERS})
