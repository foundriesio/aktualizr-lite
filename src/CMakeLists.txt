set(TARGET_EXE aktualizr-lite)
set(TARGET_LIB aktualizr_lite)
set(TARGET ${MAIN_TARGET_LIB})

set(SRC helpers.cc
        composeappmanager.cc
        rootfstreemanager.cc
        docker/restorableappengine.cc
        docker/composeappengine.cc
        docker/composeapptree.cc
        docker/composeinfo.cc
        ostree/sysroot.cc
        ostree/repo.cc
        docker/dockerclient.cc
        docker/docker.cc
        bootloader/bootloaderlite.cc
        bootloader/rollbackfactory.cc
        liteclient.cc
        yaml2json.cc
        target.cc)

set(HEADERS helpers.h
        composeappmanager.h
        rootfstreemanager.h
        docker/restorableappengine.h
        docker/composeappengine.h
        docker/composeapptree.h
        docker/composeinfo.h
        appengine.h
        ostree/sysroot.h
        ostree/repo.h
        docker/dockerclient.h
        docker/docker.h
        bootloader/bootloaderlite.h
        bootloader/rollbacks/exception.h
        bootloader/rollbacks/factory.h
        bootloader/rollbacks/generic.h
        bootloader/rollbacks/masked.h
        bootloader/rollbacks/fiovb.h
        bootloader/rollbacks/rollback.h
        liteclient.h
        yaml2json.h
        target.h)

add_executable(${TARGET_EXE} main.cc)
add_library(${TARGET} OBJECT ${SRC})
add_library(${TARGET_LIB} SHARED ${SRC} api.cc)

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
target_include_directories(${TARGET_LIB} PRIVATE ${AKLITE_DIR}/include ${INCS})

target_link_libraries(${TARGET} aktualizr_lib)
target_link_libraries(${TARGET_LIB} aktualizr_lib)
target_link_libraries(${TARGET_EXE} ${TARGET})

# TODO: consider cleaning up the overall "install" elements as it includes
# redundant targets (e.g aktualizr-secondary, aktualizr-cert-provider, etc)
install(TARGETS ${TARGET_EXE} RUNTIME DESTINATION bin COMPONENT ${TARGET_EXE})
install(TARGETS ${TARGET_LIB} LIBRARY DESTINATION lib COMPONENT ${TARGET_LIB})
install(DIRECTORY ../include/aktualizr-lite DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# enable creating clang-tidy targets for each source file (see aktualizr/CMakeLists.txt for details)
aktualizr_source_file_checks(main.cc api.cc ../include/aktualizr-lite/api.h ${SRC} ${HEADERS})
