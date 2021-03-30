set(TARGET_EXE aktualizr-lite)
set(TARGET ${MAIN_TARGET_LIB})

set(SRC helpers.cc
        compose/composeappmanager.cc
        compose/composeappengine.cc
        compose/composeapptree.cc
        compose/composeinfo.cc
        ostree/sysroot.cc
        ostree/repo.cc
        dockerclient.cc
        docker.cc
        liteclient.cc
        yaml2json.cc
        target.cc)

set(HEADERS helpers.h
        compose/composeappmanager.h
        compose/composeappengine.h
        compose/composeapptree.h
        compose/composeinfo.h
        compose/appengine.h
        ostree/sysroot.h
        ostree/repo.h
        dockerclient.h
        docker.h
        liteclient.h
        yaml2json.h
        target.h)

add_executable(${TARGET_EXE} main.cc)
add_library(${TARGET} OBJECT ${SRC})

if(ALLOW_MANUAL_ROLLBACK)
  add_definitions(-DALLOW_MANUAL_ROLLBACK)
endif(ALLOW_MANUAL_ROLLBACK)

target_compile_definitions(${TARGET} PRIVATE BOOST_LOG_DYN_LINK)
target_compile_definitions(${TARGET_EXE} PRIVATE BOOST_LOG_DYN_LINK)

set(INCS
  ${AKLITE_DIR}/src/compose
  ${AKLITE_DIR}/src/
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
