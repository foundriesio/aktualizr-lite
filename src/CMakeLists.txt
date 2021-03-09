set(TARGET_EXE aktualizr-lite)
set(TARGET ${MAIN_TARGET_LIB})

set(SRC helpers.cc
        composeappengine.cc
        docker.cc
        ostree/sysroot.cc
        composeappmanager.cc
        liteclient.cc
        ostree/repo.cc
        composeapptree.cc)

set(HEADERS
        helpers.h
        composeappengine.h
        appengine.h
        docker.h
        ostree/sysroot.h
        composeappmanager.h
        liteclient.h
        ostree/repo.h
        composeapptree.h )

add_executable(${TARGET_EXE} main.cc)
add_library(${TARGET} OBJECT ${SRC})

if(ALLOW_MANUAL_ROLLBACK)
  add_definitions(-DALLOW_MANUAL_ROLLBACK)
endif(ALLOW_MANUAL_ROLLBACK)

target_compile_definitions(${TARGET} PRIVATE BOOST_LOG_DYN_LINK)
target_compile_definitions(${TARGET_EXE} PRIVATE BOOST_LOG_DYN_LINK)

set(INCS
  ${AKTUALIZR_DIR}/include
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
aktualizr_source_file_checks(main.cc ${SRC} ${HEADERS})
