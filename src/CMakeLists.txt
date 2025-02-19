set(TARGET_EXE aktualizr-lite)
set(TARGET_LIB ${MAIN_TARGET_LIB})

set(SRC helpers.cc
        storage/stat.cc
        composeappmanager.cc
        rootfstreemanager.cc
        docker/restorableappengine.cc
        docker/composeappengine.cc
        docker/composeinfo.cc
        ostree/sysroot.cc
        ostree/repo.cc
        docker/dockerclient.cc
        docker/docker.cc
        bootloader/bootloaderlite.cc
        liteclient.cc
        yaml2json.cc
        target.cc
        appengine.cc
        cli/cli.cc
        api.cc
        aklite_client_ext.cc
        tuf/akhttpsreposource.cc
        tuf/localreposource.cc
        tuf/akrepo.cc
        daemon.cc
        aklitereportqueue.cc)

set(HEADERS helpers.h
        ../include/aktualizr-lite/storage/stat.h
        composeappmanager.h
        rootfstreemanager.h
        docker/restorableappengine.h
        docker/composeappengine.h
        docker/composeinfo.h
        appengine.h
        ostree/sysroot.h
        ostree/repo.h
        docker/dockerclient.h
        docker/docker.h
        bootloader/bootloaderlite.h
        liteclient.h
        yaml2json.h
        target.h
        downloader.h
        installer.h
        exec.h
        ../include/aktualizr-lite/cli/cli.h
        tuf/akhttpsreposource.h
        tuf/localreposource.h
        tuf/akrepo.h
        ../include/aktualizr-lite/api.h
        ../include/aktualizr-lite/aklite_client_ext.h
        ../include/aktualizr-lite/tuf/tuf.h
        daemon.h
        aklitereportqueue.h)

if(USE_COMPOSEAPP_ENGINE)
  set(SRC ${SRC} composeapp/appengine.cc)
  set(HEADERS ${HEADERS} composeapp/appengine.h)
endif(USE_COMPOSEAPP_ENGINE)

add_executable(${TARGET_EXE} main.cc)
add_library(${TARGET_LIB} SHARED ${SRC})
set_property(TARGET ${TARGET_LIB} PROPERTY CXX_STANDARD 17)

if(ALLOW_MANUAL_ROLLBACK)
  add_definitions(-DALLOW_MANUAL_ROLLBACK)
endif(ALLOW_MANUAL_ROLLBACK)

if(BUILD_P11)
  add_definitions(-DBUILD_P11)
endif(BUILD_P11)

if(AUTO_DOWNGRADE)
  add_definitions(-DAUTO_DOWNGRADE)
endif(AUTO_DOWNGRADE)

target_compile_definitions(${TARGET_EXE} PRIVATE BOOST_LOG_DYN_LINK)

set(INCS
  ${AKLITE_DIR}/src/
  ${AKLITE_DIR}/src/compose
  ${AKLITE_DIR}/include/
  ${AKTUALIZR_DIR}/include
  ${AKTUALIZR_DIR}/src/libaktualizr
  ${AKTUALIZR_DIR}/third_party/jsoncpp/include
  ${AKTUALIZR_DIR}/third_party/googletest/googletest/include
  ${GLIB_INCLUDE_DIRS}
  ${LIBOSTREE_INCLUDE_DIRS}
)

target_include_directories(${TARGET_EXE} PRIVATE ${INCS})
target_include_directories(${TARGET_LIB} PRIVATE ${AKLITE_DIR}/include ${INCS})
if(BUILD_WITH_CODE_COVERAGE_AKLITE)
  target_link_options(
    ${TARGET_LIB}
    PRIVATE "-fprofile-arcs"
    PRIVATE "-ftest-coverage"
  )
  target_compile_options(
    ${TARGET_LIB}
    PRIVATE "--coverage"
    PRIVATE "-fprofile-arcs"
    PRIVATE "-ftest-coverage"
  )
  target_link_libraries(${TARGET_LIB} gcov)
endif()

target_link_libraries(${TARGET_LIB} aktualizr_lib)
target_link_libraries(${TARGET_EXE} ${TARGET_LIB})

# TODO: consider cleaning up the overall "install" elements as it includes
# redundant targets (e.g aktualizr-secondary, aktualizr-cert-provider, etc)
install(TARGETS ${TARGET_EXE} RUNTIME DESTINATION bin COMPONENT ${TARGET_EXE})
install(TARGETS ${TARGET_LIB} LIBRARY DESTINATION lib COMPONENT ${TARGET_LIB})
install(DIRECTORY ../include/aktualizr-lite DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# enable creating clang-tidy targets for each source file (see aktualizr/CMakeLists.txt for details)
aktualizr_source_file_checks(main.cc ${SRC} ${HEADERS})
