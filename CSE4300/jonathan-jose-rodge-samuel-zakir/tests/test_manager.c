/*
 * test_manager.c - unit tests for the server manager (server_manager.c).
 *
 * The manager validates each request, finds or creates the worker for its
 * conversation, and hands the message over. Its internals are private, so
 * these tests only use the public API and observe results the way a client
 * would: through fake client sockets and the conversation history files.
 *
 * Run:  make test            (all tests)
 *       make test FILTER=xyz (only tests whose name contains "xyz")
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#include "framework.h"
#include "server_manager.h"
#include "worker_helpers.h"

/* ---- small wrappers so tests read like the protocol -------------------- */

static ServerManager *new_manager(void) {
    ServerManager *m = NULL;
    CHECK(manager_init(&m) == 0);
    CHECK(m != NULL);
    return m;
}

static int join(ServerManager *m, const char *conv, const fake_client_t *c, const char *user) {
    return manager_handle_request(m, conv, MSG_JOIN, user, NULL, c->server_fd);
}

static int say(ServerManager *m, const char *conv, const fake_client_t *c, const char *user,
               const char *msg) {
    return manager_handle_request(m, conv, MSG_SEND, user, msg, c->server_fd);
}

static int leave(ServerManager *m, const char *conv, const fake_client_t *c, const char *user) {
    return manager_handle_request(m, conv, MSG_LEAVE, user, NULL, c->server_fd);
}

/* Some tests open hundreds of sockets; macOS defaults to 256 per process. */
static void raise_fd_limit(void) {
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0);
    rlim_t want = 4096;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) {
        want = rl.rlim_max;
    }
    if (rl.rlim_cur < want) {
        rl.rlim_cur = want;
        CHECK(setrlimit(RLIMIT_NOFILE, &rl) == 0);
    }
}

/* ======================================================================
 * Lifecycle
 * ====================================================================== */

static void test_init_rejects_null_output(void) {
    CHECK(manager_init(NULL) == EINVAL);
}

static void test_shutdown_with_no_conversations(void) {
    ServerManager *m = new_manager();
    CHECK(manager_shutdown(&m) == 0);
    CHECK(m == NULL);
}

static void test_shutdown_rejects_null_handle(void) {
    CHECK(manager_shutdown(NULL) == EINVAL);
}

static void test_shutdown_of_already_shut_down_manager_is_ok(void) {
    ServerManager *m = new_manager();
    CHECK(manager_shutdown(&m) == 0);
    CHECK(manager_shutdown(&m) == 0); /* m is NULL now: nothing to do */
}

/* ======================================================================
 * Request validation
 * ====================================================================== */

static void test_rejects_missing_or_empty_fields(void) {
    ServerManager *m = new_manager();
    fake_client_t c = fake_client_new();
    int fd = c.server_fd;

    CHECK(manager_handle_request(NULL, "room", MSG_JOIN, "alice", NULL, fd) == EINVAL);
    CHECK(manager_handle_request(m, NULL, MSG_JOIN, "alice", NULL, fd) == EINVAL);
    CHECK(manager_handle_request(m, "", MSG_JOIN, "alice", NULL, fd) == EINVAL);
    CHECK(manager_handle_request(m, "room", MSG_JOIN, NULL, NULL, fd) == EINVAL);
    CHECK(manager_handle_request(m, "room", MSG_JOIN, "", NULL, fd) == EINVAL);
    CHECK(manager_handle_request(m, "room", MSG_JOIN, "alice", NULL, -1) == EINVAL);

    CHECK(manager_shutdown(&m) == 0);
}

static void test_rejects_unknown_message_type(void) {
    ServerManager *m = new_manager();
    fake_client_t c = fake_client_new();

    CHECK(manager_handle_request(m, "room", (msg_type_t)99, "alice", NULL, c.server_fd) == EINVAL);
    /* 0x1000 is what MSG_SEND turns into in any file that also includes
       <sys/socket.h> on macOS (see the note in worker_helpers.c). */
    CHECK(manager_handle_request(m, "room", (msg_type_t)0x1000, "alice", "hi", c.server_fd)
          == EINVAL);

    CHECK(manager_shutdown(&m) == 0);
}

static void test_rejects_send_without_message(void) {
    ServerManager *m = new_manager();
    fake_client_t c = fake_client_new();

    CHECK(say(m, "room", &c, "alice", NULL) == EINVAL);

    CHECK(manager_shutdown(&m) == 0);
}

static void test_conversation_id_length_limit(void) {
    char longest_ok[64], too_long[65];
    memset(longest_ok, 'a', 63);
    longest_ok[63] = '\0';
    memset(too_long, 'b', 64);
    too_long[64] = '\0';

    ServerManager *m = new_manager();
    fake_client_t c = fake_client_new();

    CHECK(join(m, longest_ok, &c, "alice") == 0);
    CHECK(join(m, too_long, &c, "alice") == ENAMETOOLONG);

    CHECK(manager_shutdown(&m) == 0);
}

static void test_rejected_request_is_not_delivered(void) {
    ServerManager *m = new_manager();
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    CHECK(join(m, "room", &alice, "alice") == 0);
    CHECK(join(m, "room", &bob, "bob") == 0);
    CHECK(manager_handle_request(m, "room", MSG_SEND, "", "sneaky", alice.server_fd) == EINVAL);

    CHECK(is_silent(bob.client_fd, QUIET_MS));
    CHECK(history_line_count("room") == 0);

    CHECK(manager_shutdown(&m) == 0);
}

/* ======================================================================
 * Routing
 * ====================================================================== */

/* Bob only receives Alice's message if all three requests reached the SAME
   worker, so this also proves the manager reuses existing conversations. */
static void test_requests_for_same_conversation_share_one_worker(void) {
    ServerManager *m = new_manager();
    fake_client_t alice = fake_client_new(), bob = fake_client_new();

    CHECK(join(m, "room", &alice, "alice") == 0);
    CHECK(join(m, "room", &bob, "bob") == 0);
    CHECK(say(m, "room", &alice, "alice", "hello") == 0);

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "sender", "alice"));
    CHECK(json_field_is(line, "message", "hello"));

    CHECK(manager_shutdown(&m) == 0);
}

static void test_different_conversations_are_isolated(void) {
    ServerManager *m = new_manager();
    fake_client_t alice = fake_client_new(), ann = fake_client_new(), bob = fake_client_new();

    CHECK(join(m, "a", &alice, "alice") == 0);
    CHECK(join(m, "a", &ann, "ann") == 0);
    CHECK(join(m, "b", &bob, "bob") == 0);
    CHECK(say(m, "a", &alice, "alice", "only for a") == 0);

    char line[BUF_SIZE];
    CHECK(recv_line(ann.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(is_silent(bob.client_fd, QUIET_MS));
    CHECK(history_line_count("b") == 0);

    CHECK(manager_shutdown(&m) == 0);
}

static void test_leave_is_routed_to_conversation(void) {
    ServerManager *m = new_manager();
    fake_client_t alice = fake_client_new(), bob = fake_client_new(), carol = fake_client_new();

    CHECK(join(m, "room", &alice, "alice") == 0);
    CHECK(join(m, "room", &bob, "bob") == 0);
    CHECK(join(m, "room", &carol, "carol") == 0);
    CHECK(leave(m, "room", &bob, "bob") == 0);
    CHECK(say(m, "room", &alice, "alice", "bye bob") == 0);

    char line[BUF_SIZE];
    CHECK(recv_line(carol.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(is_silent(bob.client_fd, QUIET_MS));

    CHECK(manager_shutdown(&m) == 0);
}

/* The header promises input strings are copied, so callers can reuse buffers. */
static void test_request_strings_are_copied(void) {
    ServerManager *m = new_manager();
    fake_client_t alice = fake_client_new(), bob = fake_client_new();
    CHECK(join(m, "room", &alice, "alice") == 0);
    CHECK(join(m, "room", &bob, "bob") == 0);

    char user[16], msg[16];
    strcpy(user, "alice");
    strcpy(msg, "original");
    CHECK(say(m, "room", &alice, user, msg) == 0);
    strcpy(user, "XXXXX");
    strcpy(msg, "XXXXXXXX");

    char line[BUF_SIZE];
    CHECK(recv_line(bob.client_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(json_field_is(line, "sender", "alice"));
    CHECK(json_field_is(line, "message", "original"));

    CHECK(manager_shutdown(&m) == 0);
}

static void test_many_conversations(void) {
    enum { ROOMS = 50 };
    raise_fd_limit();
    ServerManager *m = new_manager();
    fake_client_t senders[ROOMS], receivers[ROOMS];

    for (int r = 0; r < ROOMS; r++) {
        char conv[16];
        snprintf(conv, sizeof(conv), "room-%d", r);
        senders[r] = fake_client_new();
        receivers[r] = fake_client_new();
        CHECK(join(m, conv, &senders[r], "s") == 0);
        CHECK(join(m, conv, &receivers[r], "r") == 0);
        CHECK(say(m, conv, &senders[r], "s", conv) == 0);
    }

    for (int r = 0; r < ROOMS; r++) {
        char line[BUF_SIZE], conv[16];
        snprintf(conv, sizeof(conv), "room-%d", r);
        CHECK(recv_line(receivers[r].client_fd, line, sizeof(line), WAIT_MS) > 0);
        CHECK(json_field_is(line, "message", conv)); /* got its own room's message */
    }

    CHECK(manager_shutdown(&m) == 0);
}

/* ======================================================================
 * Shutdown
 * ====================================================================== */

static void test_shutdown_delivers_queued_messages(void) {
    enum { COUNT = 100 };
    ServerManager *m = new_manager();
    fake_client_t alice = fake_client_new();

    CHECK(join(m, "room", &alice, "alice") == 0);
    for (int i = 0; i < COUNT; i++) {
        CHECK(say(m, "room", &alice, "alice", "queued") == 0);
    }
    CHECK(manager_shutdown(&m) == 0); /* joins workers after they drain */

    CHECK(history_line_count("room") == COUNT);
}

static void test_shutdown_stops_every_conversation(void) {
    enum { ROOMS = 10 };
    ServerManager *m = new_manager();
    fake_client_t c = fake_client_new();

    for (int r = 0; r < ROOMS; r++) {
        char conv[16];
        snprintf(conv, sizeof(conv), "room-%d", r);
        CHECK(say(m, conv, &c, "alice", "hi") == 0);
    }
    /* Would hang (and time out) if any worker were not woken and joined. */
    CHECK(manager_shutdown(&m) == 0);

    for (int r = 0; r < ROOMS; r++) {
        char conv[16];
        snprintf(conv, sizeof(conv), "room-%d", r);
        CHECK(history_line_count(conv) == 1);
    }
}

/* ======================================================================
 * Concurrency
 * ====================================================================== */

enum { RACE_ROOMS = 16, RACE_THREADS = 8 };

typedef struct {
    ServerManager *m;
    fake_client_t *clients; /* clients[room * RACE_THREADS + id] */
    int id;
    atomic_int *go;
    int result;
} racer_arg_t;

static void *racer(void *arg) {
    racer_arg_t *a = arg;
    while (!atomic_load(a->go)) {
        /* spin so all threads start at the same moment */
    }
    a->result = 0;
    for (int r = 0; r < RACE_ROOMS; r++) {
        char conv[16];
        snprintf(conv, sizeof(conv), "race-%d", r);
        int rc = join(a->m, conv, &a->clients[r * RACE_THREADS + a->id], "racer");
        if (rc != 0) {
            a->result = rc;
        }
    }
    return NULL;
}

/*
 * Several threads send the FIRST request for the same brand-new conversation
 * at the same moment. Without proper locking, two threads could both see
 * "no worker yet" and create two workers for one conversation, splitting its
 * members. Every member must then receive a message sent to the room.
 */
static void test_concurrent_first_requests_create_one_worker(void) {
    raise_fd_limit();
    ServerManager *m = new_manager();
    static fake_client_t clients[RACE_ROOMS * RACE_THREADS];
    fake_client_t sender = fake_client_new();
    for (int i = 0; i < RACE_ROOMS * RACE_THREADS; i++) {
        clients[i] = fake_client_new();
    }

    atomic_int go = 0;
    pthread_t threads[RACE_THREADS];
    racer_arg_t args[RACE_THREADS];
    for (int t = 0; t < RACE_THREADS; t++) {
        args[t] = (racer_arg_t){ .m = m, .clients = clients, .id = t, .go = &go };
        CHECK(pthread_create(&threads[t], NULL, racer, &args[t]) == 0);
    }
    atomic_store(&go, 1);
    for (int t = 0; t < RACE_THREADS; t++) {
        pthread_join(threads[t], NULL);
        CHECK(args[t].result == 0);
    }

    for (int r = 0; r < RACE_ROOMS; r++) {
        char conv[16];
        snprintf(conv, sizeof(conv), "race-%d", r);
        CHECK(say(m, conv, &sender, "sender", conv) == 0);
    }
    for (int i = 0; i < RACE_ROOMS * RACE_THREADS; i++) {
        char line[BUF_SIZE], conv[16];
        snprintf(conv, sizeof(conv), "race-%d", i / RACE_THREADS);
        CHECK(recv_line(clients[i].client_fd, line, sizeof(line), WAIT_MS) > 0);
        CHECK(json_field_is(line, "message", conv));
    }

    CHECK(manager_shutdown(&m) == 0);
}

enum { PRODUCERS = 8, ROOMS_UNDER_LOAD = 4, PER_PRODUCER = 200 };

typedef struct {
    ServerManager *m;
    int sender_fd;
    int id;
} producer_arg_t;

static void *producer(void *arg) {
    producer_arg_t *p = arg;
    char conv[16];
    snprintf(conv, sizeof(conv), "load-%d", p->id % ROOMS_UNDER_LOAD);
    for (int seq = 0; seq < PER_PRODUCER; seq++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "p%d-%d", p->id, seq);
        CHECK(manager_handle_request(p->m, conv, MSG_SEND, "producer", msg, p->sender_fd) == 0);
    }
    return NULL;
}

/*
 * Many threads send through the manager into several rooms at once. Each
 * room's observer must get exactly its own producers' messages, none lost,
 * and each producer's messages in order.
 */
static void test_concurrent_requests_across_conversations(void) {
    ServerManager *m = new_manager();
    fake_client_t sender = fake_client_new();
    fake_client_t observers[ROOMS_UNDER_LOAD];
    for (int r = 0; r < ROOMS_UNDER_LOAD; r++) {
        char conv[16];
        snprintf(conv, sizeof(conv), "load-%d", r);
        observers[r] = fake_client_new();
        CHECK(join(m, conv, &observers[r], "observer") == 0);
    }

    pthread_t threads[PRODUCERS];
    producer_arg_t args[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) {
        args[i] = (producer_arg_t){ .m = m, .sender_fd = sender.server_fd, .id = i };
        CHECK(pthread_create(&threads[i], NULL, producer, &args[i]) == 0);
    }

    /* Each observer must see PRODUCERS/ROOMS producers x PER_PRODUCER messages. */
    const int per_room = (PRODUCERS / ROOMS_UNDER_LOAD) * PER_PRODUCER;
    int next_seq[PRODUCERS] = { 0 };
    for (int r = 0; r < ROOMS_UNDER_LOAD; r++) {
        for (int n = 0; n < per_room; n++) {
            char line[BUF_SIZE], msg[64];
            int id, seq;
            CHECK(recv_line(observers[r].client_fd, line, sizeof(line), WAIT_MS) > 0);
            CHECK(json_get_string(line, "message", msg, sizeof(msg)));
            CHECK(sscanf(msg, "p%d-%d", &id, &seq) == 2);
            CHECK(id % ROOMS_UNDER_LOAD == r); /* routed to the right room */
            CHECK(seq == next_seq[id]);         /* no loss or reordering */
            next_seq[id]++;
        }
        CHECK(is_silent(observers[r].client_fd, QUIET_MS));
    }

    for (int i = 0; i < PRODUCERS; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK(manager_shutdown(&m) == 0);
}

/* ======================================================================
 * Known bugs (XFAIL)
 * ====================================================================== */

/*
 * The conversation id ends up in a file name (conversation_<id>.jsonl), so an
 * id like "../x" makes the server write outside its directory. The manager is
 * the place that validates input, so it should reject these. Uses JOIN only,
 * so no file is actually written outside the test's temp directory.
 */
static void test_rejects_conversation_id_with_path_characters(void) {
    ServerManager *m = new_manager();
    fake_client_t c = fake_client_new();

    CHECK(join(m, "../escape", &c, "alice") != 0);
    CHECK(join(m, "a/b", &c, "alice") != 0);

    CHECK(manager_shutdown(&m) == 0);
}

/* ====================================================================== */

static const test_case_t tests[] = {
    /* lifecycle */
    TEST(test_init_rejects_null_output),
    TEST(test_shutdown_with_no_conversations),
    TEST(test_shutdown_rejects_null_handle),
    TEST(test_shutdown_of_already_shut_down_manager_is_ok),
    /* validation */
    TEST(test_rejects_missing_or_empty_fields),
    TEST(test_rejects_unknown_message_type),
    TEST(test_rejects_send_without_message),
    TEST(test_conversation_id_length_limit),
    TEST(test_rejected_request_is_not_delivered),
    /* routing */
    TEST(test_requests_for_same_conversation_share_one_worker),
    TEST(test_different_conversations_are_isolated),
    TEST(test_leave_is_routed_to_conversation),
    TEST(test_request_strings_are_copied),
    TEST(test_many_conversations),
    /* shutdown */
    TEST(test_shutdown_delivers_queued_messages),
    TEST(test_shutdown_stops_every_conversation),
    /* concurrency */
    TEST(test_concurrent_first_requests_create_one_worker),
    TEST(test_concurrent_requests_across_conversations),
    /* known bugs */
    TEST_XFAIL(test_rejects_conversation_id_with_path_characters,
               "ids like \"../x\" are accepted and used in the history file path"),
};

int main(int argc, char **argv) {
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]), argc, argv);
}
