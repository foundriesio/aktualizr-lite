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
	docker run --init -u $(shell id -u):$(shell id -g) --rm -v $(PWD):$(PWD) -w $(PWD) -eCCACHE_DIR=$(CCACHE_DIR) -eCC=$(CC) -eCXX=$(CXX) -eBUILD_DIR=$(BUILD_DIR) -eTARGET=$(TARGET) $(CONTAINER) make -f dev-flow.mk $@

test:
	docker run --init -u $(shell id -u):$(shell id -g) --rm -v $(PWD):$(PWD) -w $(PWD) -eTEST_LABEL=$(TEST_LABEL) -eCTEST_ARGS=$(CTEST_ARGS) -eBUILD_DIR=$(BUILD_DIR) -eGTEST_FILTER=$(GTEST_FILTER) $(CONTAINER) make -f dev-flow.mk $@
