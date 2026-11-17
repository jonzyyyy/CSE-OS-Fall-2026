# CSE 4300 multithreaded chat server
#
#   make test             build and run all unit tests
#   make test FILTER=foo  run only tests whose name contains "foo"
#   make test-asan        run tests under AddressSanitizer + UBSan (memory bugs)
#   make test-tsan        run tests under ThreadSanitizer (data races)
#   make test-all         all of the above
#   make client           build the chat client
#   make server           build the chat server
#   make clean            remove build output

CPPFLAGS += -I. -Itests -D_DEFAULT_SOURCE
CFLAGS   ?= -std=c11 -Wall -Wextra -g -O1
LDLIBS   += -pthread
SANITIZE ?=
BUILD    ?= build

SERVER_SRC := server_worker.c server_manager.c
COMMON_SRC := tests/framework.c tests/worker_helpers.c
HEADERS    := $(wildcard *.h tests/*.h)

# One test binary per tests/<name>.c listed here.
TEST_NAMES := test_worker test_manager test_client test_integration
TEST_BINS  := $(addprefix $(BUILD)/,$(TEST_NAMES))

.PHONY: all client server test test-asan test-tsan test-all clean

all: $(TEST_BINS)

$(BUILD)/%: tests/%.c $(SERVER_SRC) $(COMMON_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZE) $< $(SERVER_SRC) $(COMMON_SRC) -o $@ $(LDLIBS)

client: client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) client.c -o client $(LDLIBS)

server: server.c $(SERVER_SRC) $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) server.c $(SERVER_SRC) -o server $(LDLIBS)

# The client tests run the real client program, built into $(BUILD) with the
# same sanitizer flags as the tests, and find it through CLIENT_BIN.
$(BUILD)/client: client.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZE) client.c -o $@ $(LDLIBS)

$(BUILD)/test_client: $(BUILD)/client
$(BUILD)/test_client: CPPFLAGS += -DCLIENT_BIN='"$(abspath $(BUILD)/client)"'

# The integration tests run the real server and real clients together.
$(BUILD)/server: server.c $(SERVER_SRC) $(wildcard *.h)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZE) server.c $(SERVER_SRC) -o $@ $(LDLIBS)

$(BUILD)/test_integration: $(BUILD)/client $(BUILD)/server
$(BUILD)/test_integration: CPPFLAGS += -DCLIENT_BIN='"$(abspath $(BUILD)/client)"' \
                                       -DSERVER_BIN='"$(abspath $(BUILD)/server)"'

# Runs every test binary (even after a failure), then reports overall status.
test: $(TEST_BINS)
	@status=0; \
	for bin in $(TEST_BINS); do \
		echo "=== $$bin"; \
		./$$bin $(FILTER) || status=1; \
		echo; \
	done; \
	exit $$status

# Apple clang's sanitizer runtimes hang (ASan) or segfault (TSan) at startup
# on recent macOS, before main() runs. Run these on Linux, or pass FORCE=1 to
# try anyway with a toolchain you know works.
ifeq ($(shell uname -s),Darwin)
ifndef FORCE
SANITIZER_GUARD = @echo "Sanitizers are broken with Apple clang on this macOS; run on Linux or use FORCE=1." && false
endif
endif

test-asan:
	$(SANITIZER_GUARD)
	$(MAKE) test BUILD=build/asan \
		SANITIZE="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"

test-tsan:
	$(SANITIZER_GUARD)
	$(MAKE) test BUILD=build/tsan SANITIZE="-fsanitize=thread"

test-all: test test-asan test-tsan

clean:
	rm -rf build client server
