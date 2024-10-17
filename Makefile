CONTAINER ?= foundries/aklite-dev
CCACHE_DIR = $(shell pwd)/.ccache
BUILD_DIR ?= build-cont
TARGET ?= aklite-tests
TEST_LABEL ?= aklite
CTEST_ARGS ?= --output-on-failure
CXX ?= clang++
CC ?= clang
GTEST_FILTER ?= "*"
PKCS11_ENGINE_PATH ?= "/usr/lib/x86_64-linux-gnu/engines-3/pkcs11.so"


TASKS = config build format tidy garage-tools test-uptane-vectors
.PHONY: $(TASKS) test


all $(TASKS) config-coverage test-coverage-html:
	docker run --init -u $(shell id -u):$(shell id -g) --rm -v $(PWD):$(PWD) -w $(PWD) -eCCACHE_DIR=$(CCACHE_DIR) -eCC=$(CC) -eCXX=$(CXX) -eBUILD_DIR=$(BUILD_DIR) -eTARGET=$(TARGET) -ePKCS11_ENGINE_PATH=$(PKCS11_ENGINE_PATH) -eTEST_LABEL=$(TEST_LABEL) $(CONTAINER) make -f dev-flow.mk $@

test:
	docker run --init -u $(shell id -u):$(shell id -g) --rm -v $(PWD):$(PWD) -w $(PWD) -eTEST_LABEL=$(TEST_LABEL) -eCTEST_ARGS=$(CTEST_ARGS) -eBUILD_DIR=$(BUILD_DIR) -eGTEST_FILTER=$(GTEST_FILTER) $(CONTAINER) make -f dev-flow.mk $@

# Commands to be used inside the aklite-e2e-test docker
BIN ?= build/src/aktualizr-lite
SOTA_DIR = /var/sota
DEVICE_FACTORY ?= ${FACTORY}
DEVICE_TOKEN ?= ${AUTH_TOKEN}
DEVICE_TAG ?= main

${SOTA_DIR}:
	mkdir -p ${SOTA_DIR}/reset-apps
	mkdir -p ${SOTA_DIR}/compose-apps

register: ${SOTA_DIR}
	DEVICE_FACTORY=${DEVICE_FACTORY} lmp-device-register -T ${DEVICE_TOKEN} --start-daemon 0 -d ${SOTA_DIR} -t ${DEVICE_TAG}

unregister:
	@rm -rf ${SOTA_DIR}/sql.db
	@rm ${SOTA_DIR}/*.toml

check:
	${BIN} check

update:
	@ostree config set core.mode bare-user
	${BIN} pull
	@ostree config set core.mode bare-user-only
	${BIN} install

reboot:
	@rm -f /var/run/aktualizr-session/need_reboot

run:
	${BIN} run
