# CSE 4300 multithreaded chat server
#
#   make test             build and run the unit tests
#   make test FILTER=foo  run only tests whose name contains "foo"
#   make test-asan        run tests under AddressSanitizer + UBSan (memory bugs)
#   make test-tsan        run tests under ThreadSanitizer (data races)
#   make test-all         all of the above
#   make clean            remove build output

CPPFLAGS += -I. -Itests -D_DEFAULT_SOURCE
CFLAGS   ?= -std=c11 -Wall -Wextra -g -O1
LDLIBS   += -pthread
SANITIZE ?=
BUILD    ?= build

SERVER_SRC := server_worker.c
TEST_SRC   := tests/test_worker.c tests/framework.c tests/worker_helpers.c
HEADERS    := server_worker.h tests/framework.h tests/worker_helpers.h

TEST_BIN := $(BUILD)/test_worker

.PHONY: all test test-asan test-tsan test-all clean

all: $(TEST_BIN)

$(TEST_BIN): $(SERVER_SRC) $(TEST_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZE) $(SERVER_SRC) $(TEST_SRC) -o $@ $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN) $(FILTER)

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
	rm -rf build
