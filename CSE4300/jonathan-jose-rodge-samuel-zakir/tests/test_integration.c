/*
 * test_integration.c - end-to-end tests: real server + real clients.
 *
 * Every other test file fakes one side. These run the actual ./server and
 * several actual ./client programs and check that a message typed into one
 * client shows up on another:
 *
 *   test --stdin--> ./client alice --TCP--> ./server --TCP--> ./client bob --stdout--> test
 *
 * The server is started on port 0 (the OS picks a free port) and the test
 * reads the real port from its "listening on port N" line. The server runs
 * in the test's temporary directory, so conversation_*.jsonl files land
 * there and can be checked with the history helpers.
 *
 * CLIENT_BIN and SERVER_BIN are set by the Makefile.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "framework.h"
#include "worker_helpers.h" /* recv_line, is_silent, history helpers */

#ifndef CLIENT_BIN
#define CLIENT_BIN "build/client"
#endif
#ifndef SERVER_BIN
#define SERVER_BIN "build/server"
#endif

/* ======================================================================
 * Harness
 * ====================================================================== */

typedef struct {
    pid_t pid;
    int input_fd;  /* write here = typing into the program */
    int output_fd; /* read here  = what the program prints */
} proc_t;

static proc_t spawn(const char *path, char *const argv[]) {
    int in[2], out[2];
    CHECK(pipe(in) == 0 && pipe(out) == 0);

    proc_t p;
    p.pid = fork();
    CHECK(p.pid >= 0);
    if (p.pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        close(in[0]); close(in[1]);
        close(out[0]); close(out[1]);
        execv(path, argv);
        perror("execv");
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    p.input_fd = in[1];
    p.output_fd = out[0];
    return p;
}

static void stop(proc_t *p) {
    kill(p->pid, SIGKILL); /* no-op if it already exited */
    waitpid(p->pid, NULL, 0);
    close(p->input_fd);
    close(p->output_fd);
}

static int is_running(pid_t pid) {
    return waitpid(pid, NULL, WNOHANG) == 0;
}

/* Read output until a line containing `text` appears. */
static int output_contains(const proc_t *p, const char *text) {
    char line[BUF_SIZE];
    while (recv_line(p->output_fd, line, sizeof(line), WAIT_MS) > 0) {
        if (strstr(line, text) != NULL) {
            return 1;
        }
    }
    return 0;
}

static proc_t start_server(int *port) {
    char *argv[] = { "server", "0", NULL };
    proc_t s = spawn(SERVER_BIN, argv);

    char line[BUF_SIZE];
    CHECK(recv_line(s.output_fd, line, sizeof(line), WAIT_MS) > 0);
    CHECK(sscanf(line, "listening on port %d", port) == 1);
    return s;
}

static void type_line(const proc_t *c, const char *text) {
    size_t len = strlen(text);
    CHECK(write(c->input_fd, text, len) == (ssize_t)len);
    CHECK(write(c->input_fd, "\n", 1) == 1);
}

/* Poll until some line of the conversation's history contains `text`. */
static int history_contains(const char *conv, const char *text) {
    char line[BUF_SIZE];
    for (int waited = 0; waited < WAIT_MS; waited += 10) {
        for (int i = 0; read_history_line(conv, i, line, sizeof(line)); i++) {
            if (strstr(line, text) != NULL) {
                return 1;
            }
        }
        sleep_ms(10);
    }
    return 0;
}

/*
 * Start a client and wait until the server has really registered it.
 *
 * The client connecting (or printing "Joined") only means its JOIN was SENT.
 * So the new client also says "<user> is here" and we wait for that line to
 * reach the history file. One connection's requests are handled in order and
 * the conversation's worker processes its queue in order, so once the line is
 * in history, this client's JOIN has been processed too: it is now listening.
 * Clients already in the room receive the line; output_contains skips it.
 */
static proc_t start_client(int port, const char *user, const char *conv) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    char *argv[] = { "client", (char *)user, (char *)conv, "127.0.0.1", port_str, NULL };
    proc_t c = spawn(CLIENT_BIN, argv);

    char hello[96];
    snprintf(hello, sizeof(hello), "%s is here", user);
    type_line(&c, hello);
    CHECK(history_contains(conv, hello));
    return c;
}

/* ======================================================================
 * Tests
 * ====================================================================== */

static void test_message_reaches_other_client(void) {
    int port;
    proc_t server = start_server(&port);
    proc_t alice = start_client(port, "alice", "01");
    proc_t bob = start_client(port, "bob", "01");

    type_line(&alice, "hello bob");
    CHECK(output_contains(&bob, "alice: hello bob"));

    stop(&alice); stop(&bob); stop(&server);
}

static void test_replies_flow_both_ways(void) {
    int port;
    proc_t server = start_server(&port);
    proc_t alice = start_client(port, "alice", "01");
    proc_t bob = start_client(port, "bob", "01");

    type_line(&alice, "ping");
    CHECK(output_contains(&bob, "alice: ping"));
    type_line(&bob, "pong");
    CHECK(output_contains(&alice, "bob: pong"));

    stop(&alice); stop(&bob); stop(&server);
}

static void test_late_joiner_sees_history(void) {
    int port;
    proc_t server = start_server(&port);
    proc_t alice = start_client(port, "alice", "01");

    type_line(&alice, "said before bob arrived");
    CHECK(history_contains("01", "said before bob arrived"));

    proc_t bob = start_client(port, "bob", "01");
    CHECK(output_contains(&bob, "alice: said before bob arrived"));

    stop(&alice); stop(&bob); stop(&server);
}

static void test_conversations_are_isolated(void) {
    int port;
    proc_t server = start_server(&port);
    proc_t alice = start_client(port, "alice", "01");
    proc_t carol = start_client(port, "carol", "01");
    proc_t bob = start_client(port, "bob", "02");

    type_line(&alice, "only for room 01");
    CHECK(output_contains(&carol, "alice: only for room 01"));
    CHECK(is_silent(bob.output_fd, QUIET_MS));

    stop(&alice); stop(&carol); stop(&bob); stop(&server);
}

/* The client escapes quotes; the server must keep the message valid JSON all
   the way to the history file and back out to the other client. */
static void test_quotes_survive_round_trip(void) {
    int port;
    proc_t server = start_server(&port);
    proc_t alice = start_client(port, "alice", "01");
    proc_t bob = start_client(port, "bob", "01");

    type_line(&alice, "she said \"hi\" \\o/");
    CHECK(output_contains(&bob, "alice: she said \"hi\" \\o/"));

    char line[BUF_SIZE];
    CHECK(history_contains("01", "she said"));
    int last = history_line_count("01") - 1;
    CHECK(read_history_line("01", last, line, sizeof(line)));
    CHECK(json_field_is(line, "message", "she said \"hi\" \\o/"));

    stop(&alice); stop(&bob); stop(&server);
}

/* A client killed without /quit must not crash the server (SIGPIPE) or stop
   the conversation working for everyone else. */
static void test_server_survives_client_crash(void) {
    int port;
    proc_t server = start_server(&port);
    proc_t alice = start_client(port, "alice", "01");
    proc_t bob = start_client(port, "bob", "01");
    proc_t carol = start_client(port, "carol", "01");

    stop(&alice); /* killed: no LEAVE sent */

    for (int i = 0; i < 5; i++) { /* several writes to the dead socket */
        char msg[32], expected[64];
        snprintf(msg, sizeof(msg), "still here %d", i);
        snprintf(expected, sizeof(expected), "bob: %s", msg);
        type_line(&bob, msg);
        CHECK(output_contains(&carol, expected));
    }
    CHECK(is_running(server.pid));

    stop(&bob); stop(&carol); stop(&server);
}

static void test_quit_then_rejoin(void) {
    int port;
    proc_t server = start_server(&port);
    proc_t alice = start_client(port, "alice", "01");
    proc_t bob = start_client(port, "bob", "01");

    type_line(&bob, "/quit");
    waitpid(bob.pid, NULL, 0);
    close(bob.input_fd); close(bob.output_fd);

    type_line(&alice, "after bob left");
    CHECK(history_contains("01", "after bob left"));

    proc_t bob2 = start_client(port, "bob", "01");
    CHECK(output_contains(&bob2, "alice: after bob left")); /* via history */
    CHECK(is_running(server.pid));

    stop(&alice); stop(&bob2); stop(&server);
}

static const test_case_t tests[] = {
    TEST(test_message_reaches_other_client),
    TEST(test_replies_flow_both_ways),
    TEST(test_late_joiner_sees_history),
    TEST(test_conversations_are_isolated),
    TEST(test_quotes_survive_round_trip),
    TEST(test_server_survives_client_crash),
    TEST(test_quit_then_rejoin),
};

int main(int argc, char **argv) {
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]), argc, argv);
}
