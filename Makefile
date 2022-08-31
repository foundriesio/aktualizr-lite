CONTAINER ?= foundries/aklite-dev
CCACHE_DIR = $(shell pwd)/.ccache
BUILD_DIR ?= build-cont
TARGET ?= aklite-tests
TEST_LABEL ?= aklite
CTEST_ARGS ?= --output-on-failure
CXX ?= clang++
CC ?= clang
GTEST_FILTER ?= "*"


TASKS = config build format tidy garage-tools
.PHONY: $(TASKS) test


all $(TASKS):
	docker run -u $(shell id -u):$(shell id -g) --rm -v $(PWD):$(PWD) -w $(PWD) -eCCACHE_DIR=$(CCACHE_DIR) -eCC=$(CC) -eCXX=$(CXX) -eBUILD_DIR=$(BUILD_DIR) -eTARGET=$(TARGET) $(CONTAINER) make -f dev-flow.mk $@

test:
	docker run -u $(shell id -u):$(shell id -g) --rm -v $(PWD):$(PWD) -w $(PWD) $(CONTAINER) TEST_LABEL=$(TEST_LABEL) CTEST_ARGS=$(CTEST_ARGS) BUILD_DIR=$(BUILD_DIR) GTEST_FILTER=$(GTEST_FILTER) make -f dev-flow.mk $@
