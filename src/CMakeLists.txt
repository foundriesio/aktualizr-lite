set(TARGET_EXE aktualizr-lite)
set(TARGET ${MAIN_TARGET_LIB})

set(SRC helpers.cc composeappmanager.cc docker.cc composeapp.cc ostree.cc)
set(HEADERS helpers.h composeappmanager.h docker.h composeapp.h ostree.h)

add_executable(${TARGET_EXE} main.cc)
add_library(${TARGET} OBJECT ${SRC})

if(BUILD_WITH_CLANG_TIDY)
  # Build the given target sources with clang-tidy
  set_target_properties(${TARGET}
    PROPERTIES CXX_CLANG_TIDY
  # TODO: consider adding clang-tidy options that it makes sense to include here
  # TODO: if all clang-tidy checks are on then lots of errors appears
  "${CLANG_TIDY};-quiet;-checks=-*,-boost*,clang-analyzer-*,-clang-analyzer-cplusplus*;-header-filter=\(${CMAKE_SOURCE_DIR}|\\.\\.\)/src/.h"
  #  "${CLANG_TIDY_CMD};-checks=*;-quiet;-header-filter=\(${CMAKE_SOURCE_DIR}|\\.\\.\)/src/.*"
  )

endif(BUILD_WITH_CLANG_TIDY)

target_compile_definitions(${TARGET} PRIVATE BOOST_LOG_DYN_LINK)
target_compile_definitions(${TARGET_EXE} PRIVATE BOOST_LOG_DYN_LINK)

set(INCS
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  ${AKTUALIZR_DIR}/third_party/googletest/googletest/include
  ${GLIB_INCLUDE_DIRS}
  ${LIBOSTREE_INCLUDE_DIRS}
)

target_include_directories(${TARGET} PRIVATE ${INCS})
target_include_directories(${TARGET_EXE} PRIVATE ${INCS})

target_link_libraries(${TARGET} aktualizr_lib)
target_link_libraries(${TARGET_EXE} ${TARGET})


# TODO: consider cleaning up the overall "install" elements as it includes
# redundant targets (e.g aktualizr-secondary, aktualizr-cert-provider, etc)
install(TARGETS ${TARGET_EXE} RUNTIME DESTINATION bin COMPONENT ${TARGET_EXE})

# enable creating clang-tidy targets for each source file (see aktualizr/CMakeLists.txt for details)
aktualizr_source_file_checks(${SRC} ${HEADERS})
