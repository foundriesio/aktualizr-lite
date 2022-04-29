CONTAINER ?= foundries/aklite-dev
CCACHE_DIR = $(shell pwd)/.ccache
BUILD_DIR ?= build-cont
TARGET ?= aklite-tests
TEST_LABEL ?= aklite
CTEST_ARGS ?= --output-on-failure
CXX ?= clang++
CC ?= clang
GTEST_FILTER ?= "*"


TASKS = config build format tidy
.PHONY: $(TASKS) test


all $(TASKS):
	docker run -u $(shell id -u):$(shell id -g) --rm -v $(PWD):$(PWD) -w $(PWD) -eCCACHE_DIR=$(CCACHE_DIR) -eCC=$(CC) -eCXX=$(CXX) -eBUILD_DIR=$(BUILD_DIR) -eTARGET=$(TARGET) foundries/aklite-dev make -f dev-flow.mk $@

test:
	docker run --privileged  --entrypoint="wrapdocker"  --rm -v $(PWD):$(PWD) -w $(PWD) $(CONTAINER) sudo -u testuser TEST_LABEL=$(TEST_LABEL) CTEST_ARGS=$(CTEST_ARGS) BUILD_DIR=$(BUILD_DIR) GTEST_FILTER=$(GTEST_FILTER) make -f dev-flow.mk $@
