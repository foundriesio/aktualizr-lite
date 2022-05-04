.PHONY: config build test format tidy

CCACHE_DIR = $(shell pwd)/.ccache
BUILD_DIR ?= build
TARGET ?= aklite-tests
TEST_LABEL ?= aklite
CTEST_ARGS ?= --output-on-failure
CXX ?= clang++
CC ?= clang

all: config build

config:
	cmake -S . -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=Debug -DBUILD_P11=ON -GNinja -DCMAKE_CXX_COMPILER=${CXX} -DCMAKE_C_COMPILER=${CC} -DOFFLINE_UPDATE=ON

build:
	cmake --build ${BUILD_DIR} --target ${TARGET}

format:
	cmake --build ${BUILD_DIR} --target $@

tidy:
	cmake --build $(BUILD_DIR) --target $(shell cmake --build build --target help | grep aktualizr_clang_tidy-src- | cut -d: -f1)


test:
	cd ${BUILD_DIR} && ctest -L ${TEST_LABEL} -j $(shell nproc) ${CTEST_ARGS}
