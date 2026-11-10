/*
 * test_worker.c - unit tests for the conversation worker thread.
 *
 * Each test plays the manager: it creates a worker, connects fake clients
 * (see worker_helpers.h), enqueues JOIN / SEND / LEAVE messages, and checks
 * what each client receives and what is written to the history file.
 *
 * Run:  make test            (all tests)
 *       make test FILTER=xyz (only tests whose name contains "xyz")
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "framework.h"
#include "server_worker.h"
#include "worker_helpers.h"

/* ======================================================================
 * Core behaviour
 * ====================================================================== */

static void test_send_is_broadcast_to_other_members(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_send(w, &alice, "alice", "hello");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "sender", "alice"));
    CHECK(json_field_is(line, "message", "hello"));

    worker_stop(w);
}

static void test_sender_does_not_receive_own_message(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_send(w, &alice, "alice", "hello");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(is_silent(alice.client_fd, QUIET_MS));

    worker_stop(w);
}

static void test_send_appends_to_history_file(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new();

    client_join(w, &alice, "alice");
    client_send(w, &alice, "alice", "hello");

    CHECK(wait_for_history_lines("room", 1, WAIT_MS) == 1);
    char line[BUF_SIZE], timestamp[64];
    CHECK(read_history_line("room", 0, line, sizeof(line)));
    CHECK(json_field_is(line, "sender", "alice"));
    CHECK(json_field_is(line, "message", "hello"));
    /* timestamp is written as a number, so it is not a JSON string */
    CHECK(!json_get_string(line, "timestamp", timestamp, sizeof(timestamp)));
    CHECK(strstr(line, "\"timestamp\"") != NULL);

    worker_stop(w);
}

static void test_late_joiner_receives_full_history(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), carol = fake_client_new();

    client_join(w, &alice, "alice");
    client_send(w, &alice, "alice", "first");
    client_send(w, &alice, "alice", "second");
    CHECK(wait_for_history_lines("room", 2, WAIT_MS) == 2);

    client_join(w, &carol, "carol");

    char line[BUF_SIZE];
    CHECK(recv_line(carol.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "message", "first"));
    CHECK(recv_line(carol.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "message", "second"));
    CHECK(is_silent(carol.client_fd, QUIET_MS));

    worker_stop(w);
}

static void test_join_with_no_history_sends_nothing(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new();

    client_join(w, &alice, "alice");
    CHECK(is_silent(alice.client_fd, QUIET_MS));

    worker_stop(w);
}

static void test_client_that_left_receives_nothing(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new(),
                  carol = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_join(w, &carol, "carol");
    client_leave(w, &bob, "bob");
    client_send(w, &alice, "alice", "bob is gone");

    char line[BUF_SIZE];
    CHECK(recv_line(carol.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(is_silent(bob.client_fd, QUIET_MS));

    worker_stop(w);
}

static void test_messages_arrive_in_order(void) {
    enum { COUNT = 50 };
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    for (int i = 0; i < COUNT; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "msg-%d", i);
        client_send(w, &alice, "alice", msg);
    }

    for (int i = 0; i < COUNT; i++) {
        char line[BUF_SIZE], expected[32];
        snprintf(expected, sizeof(expected), "msg-%d", i);
        CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
        CHECK(json_field_is(line, "message", expected));
    }

    worker_stop(w);
}

static void test_conversations_are_isolated(void) {
    worker_t *room_a = worker_create("a");
    worker_t *room_b = worker_create("b");
    CHECK(room_a != NULL && room_b != NULL);
    fake_client_t alice = fake_client_new(), ann = fake_client_new(),
                  bob = fake_client_new();

    client_join(room_a, &alice, "alice");
    client_join(room_a, &ann, "ann");
    client_join(room_b, &bob, "bob");
    client_send(room_a, &alice, "alice", "only for room a");

    char line[BUF_SIZE];
    CHECK(recv_line(ann.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(is_silent(bob.client_fd, QUIET_MS));
    CHECK(history_line_count("a") == 1);
    CHECK(history_line_count("b") == 0);

    worker_stop(room_a);
    worker_stop(room_b);
}

/* ======================================================================
 * Edge cases
 * ====================================================================== */

static void test_duplicate_join_does_not_duplicate_delivery(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_join(w, &bob, "bob");
    client_send(w, &alice, "alice", "once");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(is_silent(bob.client_fd, QUIET_MS));

    worker_stop(w);
}

static void test_leave_by_unknown_client_is_ignored(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new(),
                  stranger = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_leave(w, &stranger, "stranger");
    client_send(w, &alice, "alice", "still here?");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);

    worker_stop(w);
}

static void test_send_with_null_message_is_ignored(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_send(w, &alice, "alice", NULL);
    client_send(w, &alice, "alice", "real");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "message", "real"));
    CHECK(wait_for_history_lines("room", 1, WAIT_MS) == 1);
    sleep_ms(QUIET_MS);
    CHECK(history_line_count("room") == 1);

    worker_stop(w);
}

static void test_join_beyond_max_clients_is_rejected(void) {
    fake_client_t clients[MAX_CLIENTS + 1];
    worker_t *w = worker_create("room");
    CHECK(w != NULL);

    for (int i = 0; i <= MAX_CLIENTS; i++) {
        clients[i] = fake_client_new();
        client_join(w, &clients[i], "user");
    }
    client_send(w, &clients[0], "user", "full house");

    /* Members 1..MAX_CLIENTS-1 receive it; the extra client never got in. */
    char line[BUF_SIZE];
    for (int i = 1; i < MAX_CLIENTS; i++) {
        CHECK(recv_line(clients[i].client_fd, line, sizeof(line), WAIT_MS) > 0);
    }
    CHECK(is_silent(clients[MAX_CLIENTS].client_fd, QUIET_MS));

    worker_stop(w);
}

static void test_stop_processes_remaining_queue(void) {
    enum { COUNT = 100 };
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new();

    for (int i = 0; i < COUNT; i++) {
        client_send(w, &alice, "alice", "queued");
    }
    worker_stop(w); /* must drain the queue before the thread exits */

    CHECK(history_line_count("room") == COUNT);
}

/* ======================================================================
 * Concurrency
 * ====================================================================== */

enum { PRODUCERS = 8, PER_PRODUCER = 200 };

typedef struct {
    worker_t *w;
    int sender_fd;
    int id;
} producer_arg_t;

static void *producer(void *arg) {
    producer_arg_t *p = arg;
    for (int seq = 0; seq < PER_PRODUCER; seq++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "p%d-%d", p->id, seq);
        enqueue(p->w, MSG_SEND, p->sender_fd, "producer", msg);
    }
    return NULL;
}

/*
 * Many threads enqueue onto the same worker at once (as the manager would
 * under load). Every message must arrive exactly once, and each producer's
 * messages must stay in the order that producer sent them.
 */
static void test_concurrent_enqueue_loses_nothing(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    /* All producers share one sender fd, which the worker never writes to,
       so only the observer's socket fills up (and the test drains it). */
    fake_client_t sender = fake_client_new(), observer = fake_client_new();
    client_join(w, &observer, "observer");

    pthread_t threads[PRODUCERS];
    producer_arg_t args[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) {
        args[i] = (producer_arg_t){ .w = w, .sender_fd = sender.server_fd, .id = i };
        CHECK(pthread_create(&threads[i], NULL, producer, &args[i]) == 0);
    }

    int next_seq[PRODUCERS] = { 0 };
    for (int n = 0; n < PRODUCERS * PER_PRODUCER; n++) {
        char line[BUF_SIZE], msg[64];
        int id, seq;
        CHECK(recv_line(observer.client_fd, line, sizeof(line), WAIT_MS) > 0);
        CHECK(json_get_string(line, "message", msg, sizeof(msg)));
        CHECK(sscanf(msg, "p%d-%d", &id, &seq) == 2);
        CHECK(id >= 0 && id < PRODUCERS);
        CHECK(seq == next_seq[id]); /* no loss, duplication, or reordering */
        next_seq[id]++;
    }
    CHECK(is_silent(observer.client_fd, QUIET_MS));

    for (int i = 0; i < PRODUCERS; i++) {
        pthread_join(threads[i], NULL);
    }
    worker_stop(w);
    CHECK(history_line_count("room") == PRODUCERS * PER_PRODUCER);
}

/* ======================================================================
 * Known bugs (XFAIL): these document current behaviour that is wrong.
 * When a fix lands, the runner reports XPASS; then change TEST_XFAIL to TEST.
 * ====================================================================== */

static void test_message_with_quotes_is_valid_json(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_send(w, &alice, "alice", "she said \"hi\"\\");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "message", "she said \"hi\"\\"));

    worker_stop(w);
}

static void test_long_message_is_not_truncated(void) {
    enum { LEN = 5000 };
    static char big[LEN + 1];
    memset(big, 'x', LEN);
    big[LEN] = '\0';

    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_send(w, &alice, "alice", big);

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "message", big));

    worker_stop(w);
}

static void test_disconnected_client_does_not_crash_worker(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new(),
                  carol = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_join(w, &carol, "carol");
    fake_client_disconnect(&bob); /* bob's app closes without sending LEAVE */

    client_send(w, &alice, "alice", "first");
    client_send(w, &alice, "alice", "second");

    char line[BUF_SIZE];
    CHECK(recv_line(carol.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "message", "first"));
    CHECK(recv_line(carol.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "message", "second"));

    worker_stop(w);
}

static void test_send_without_join_does_not_subscribe(void) {
    worker_t *w = worker_create("room");
    CHECK(w != NULL);
    fake_client_t alice = fake_client_new(), bob = fake_client_new(),
                  outsider = fake_client_new();

    client_join(w, &alice, "alice");
    client_join(w, &bob, "bob");
    client_send(w, &outsider, "outsider", "drive-by message");
    client_send(w, &alice, "alice", "members only");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(is_silent(outsider.client_fd, QUIET_MS));

    worker_stop(w);
}

/* ====================================================================== */

static const test_case_t tests[] = {
    /* core behaviour */
    TEST(test_send_is_broadcast_to_other_members),
    TEST(test_sender_does_not_receive_own_message),
    TEST(test_send_appends_to_history_file),
    TEST(test_late_joiner_receives_full_history),
    TEST(test_join_with_no_history_sends_nothing),
    TEST(test_client_that_left_receives_nothing),
    TEST(test_messages_arrive_in_order),
    TEST(test_conversations_are_isolated),
    /* edge cases */
    TEST(test_duplicate_join_does_not_duplicate_delivery),
    TEST(test_leave_by_unknown_client_is_ignored),
    TEST(test_send_with_null_message_is_ignored),
    TEST(test_join_beyond_max_clients_is_rejected),
    TEST(test_stop_processes_remaining_queue),
    /* concurrency */
    TEST(test_concurrent_enqueue_loses_nothing),
    /* known bugs */
    TEST_XFAIL(test_message_with_quotes_is_valid_json,
               "quotes/backslashes are not escaped in JSON output (handle_send)"),
    TEST_XFAIL(test_long_message_is_not_truncated,
               "messages over ~4KB are cut off and lose their newline (4096-byte buffer in handle_send)"),
    TEST_XFAIL(test_disconnected_client_does_not_crash_worker,
               "write() to a closed socket raises SIGPIPE, killing the whole server"),
    TEST_XFAIL(test_send_without_join_does_not_subscribe,
               "SEND silently adds the sender as a member (add_client in handle_send); confirm intended design"),
};

int main(int argc, char **argv) {
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]), argc, argv);
}
