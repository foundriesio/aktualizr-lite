set(TARGET aktualizr-lite)
set(SRC main.cc helpers.cc)
set(HEADERS helpers.h)

add_executable(${TARGET} ${SRC})

if(CLANG_TIDY)
set_target_properties(${TARGET}
  PROPERTIES CXX_CLANG_TIDY
# TODO: consider adding clang-tidy options that it makes sense to include here
  clang-tidy-6.0;-checks=*;-warnings-as-errors=*
)
endif(CLANG_TIDY)

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
