set(TARGET aktualizr-lite)
set(SRC main.cc helpers.cc composeappmanager.cc docker.cc)
set(HEADERS helpers.h composeappmanager.h docker.h)

add_executable(${TARGET} ${SRC})

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

if(BUILD_DOCKERAPP)
  target_compile_definitions(${TARGET} PRIVATE BUILD_DOCKERAPP)
endif(BUILD_DOCKERAPP)

target_compile_definitions(${TARGET} PRIVATE BOOST_LOG_DYN_LINK)

target_include_directories(${TARGET} PRIVATE
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  ${AKTUALIZR_DIR}/third_party/googletest/googletest/include
  ${GLIB_INCLUDE_DIRS}
  ${LIBOSTREE_INCLUDE_DIRS}
)

target_link_libraries(${TARGET} aktualizr_lib)


# TODO: consider cleaning up the overall "install" elements as it includes
# redundant targets (e.g aktualizr-secondary, aktualizr-cert-provider, etc)
install(TARGETS ${TARGET} RUNTIME DESTINATION bin COMPONENT ${TARGET})

# enable creating clang-tidy targets for each source file (see aktualizr/CMakeLists.txt for details)
aktualizr_source_file_checks(${SRC} ${HEADERS})
