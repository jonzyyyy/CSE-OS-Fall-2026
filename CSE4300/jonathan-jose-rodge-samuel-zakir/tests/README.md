# Tests

## Running

From the team folder (`CSE4300/jonathan-jose-rodge-samuel-zakir`):

```sh
make test                  # build and run all unit tests
make test FILTER=history   # only tests whose name contains "history"
make test-asan             # AddressSanitizer + UBSan (memory errors)   - Linux only
make test-tsan             # ThreadSanitizer (data races)               - Linux only
make clean
```

The sanitizer targets don't work with Apple clang on current macOS: the runtime
hangs or crashes before `main()`. Run them on Linux, e.g. the course VM.

## Layout

| File | Purpose |
|---|---|
| `framework.h/.c` | Minimal test runner: `CHECK`, `TEST`, `TEST_XFAIL` |
| `worker_helpers.h/.c` | Fake clients, enqueue helpers, line/JSON/history readers |
| `test_worker.c` | Unit tests for the worker thread (`server_worker.c`) |

New components get their own file, e.g. `test_manager.c`, plus a binary target in the `Makefile`.

## How the worker tests work

Each test plays the manager. It creates a worker, then sets up fake clients
with `socketpair()`: the worker writes to one end and the test reads the other
end, exactly as a TCP client would receive data. It then enqueues
JOIN / SEND / LEAVE messages and checks what each client receives and what is
written to `conversation_<id>.jsonl`.

The runner forks every test into its own process and temporary directory with a
timeout, so:

- a crash (e.g. SIGPIPE) fails only that test and reports the signal
- a deadlock is reported as a timeout instead of hanging the suite
- history files never leak between tests

## Result types

| Result | Meaning |
|---|---|
| `PASS` | Behaves as specified |
| `FAIL` | Regression: something that used to work is broken |
| `XFAIL` | Known bug, documented by a test that is expected to fail |
| `XPASS` | A known bug now passes. Change `TEST_XFAIL` to `TEST` |

`make test` succeeds only when there are no `FAIL` or `XPASS` results.

## Writing a test

```c
static void test_something(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_send(w, &alice, "alice", "hi");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "message", "hi"));

    worker_stop(w);
}
```

Then add `TEST(test_something)` to the `tests[]` table at the bottom of the file.

Guidelines:
- Name tests after the behaviour they check (`test_left_client_receives_nothing`).
- One behaviour per test.
- Never use fixed sleeps to wait for something to happen. Wait on an outcome
  instead: `recv_line` with `WAIT_MS`, or `wait_for_history_lines`. Use
  `is_silent(fd, QUIET_MS)` only to check that something did *not* happen.
