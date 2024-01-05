.PHONY: config build test format tidy install

CCACHE_DIR = $(shell pwd)/.ccache
BUILD_DIR ?= build
TARGET ?= aklite-tests
TEST_LABEL ?= aklite
CTEST_ARGS ?= --output-on-failure
CXX ?= clang++
CC ?= clang
GTEST_FILTER ?= "*"
PKCS11_ENGINE_PATH ?= "/usr/lib/x86_64-linux-gnu/engines-3/pkcs11.so"
EXTRA_CMAKE_CONFIG_ARGS ?= -DUSE_COMPOSEAPP_ENGINE=ON -DBUILD_AKLITE_OFFLINE=ON

all: config build

config:
	cmake -S . -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=Debug -DBUILD_P11=ON -GNinja -DCMAKE_CXX_COMPILER=${CXX} -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_FLAGS="-Wno-error=deprecated-declarations" -DPKCS11_ENGINE_PATH=${PKCS11_ENGINE_PATH} ${EXTRA_CMAKE_CONFIG_ARGS}

build:
	cmake --build ${BUILD_DIR} --target ${TARGET}

format:
	cmake --build ${BUILD_DIR} --target $@

tidy:
	cmake --build $(BUILD_DIR) --target $(shell cmake --build $(BUILD_DIR) --target help | grep aktualizr_clang_tidy-src- | cut -d: -f1)


test:
	cd ${BUILD_DIR} && GTEST_FILTER=${GTEST_FILTER} ctest -L ${TEST_LABEL} -j $(shell nproc) ${CTEST_ARGS}

test-uptane-vectors:
	cmake -B aktualizr/${BUILD_DIR} -DCMAKE_BUILD_TYPE=Debug -DWARNING_AS_ERROR=OFF
	cd aktualizr/${BUILD_DIR} && make -j $(shell nproc) aktualizr_uptane_vector_tests test ARGS="-R test_uptane_vectors"

install:
	cmake --build ${BUILD_DIR} --target $@
	cp -r aktualizr/third_party/jsoncpp/include/json /usr/include

custom-client:
	cmake -S examples/custom-client-cxx -B ${BUILD_DIR}-custom -GNinja -DCMAKE_CXX_COMPILER=${CXX} -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_FLAGS="-I $(CURDIR)/include -I $(CURDIR)/aktualizr/third_party/jsoncpp/include/ -L "$(CURDIR)/${BUILD_DIR}/aktualizr/src/libaktualizr/" -L $(CURDIR)/${BUILD_DIR}/src"
	cmake --build ${BUILD_DIR}-custom --target all

garage-tools:
	cmake -S . -B ${BUILD_DIR}-garage -DCMAKE_BUILD_TYPE=Debug -GNinja -DBUILD_P11=ON -DBUILD_SOTA_TOOLS=ON -DCMAKE_CXX_COMPILER=${CXX} -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_FLAGS="-Wno-error=deprecated-declarations" -DPKCS11_ENGINE_PATH=${PKCS11_ENGINE_PATH}
	cmake --build ${BUILD_DIR}-garage --target all
