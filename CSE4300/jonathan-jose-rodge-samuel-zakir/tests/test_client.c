/*
 * test_client.c - black-box tests for the chat client (client.c).
 *
 * client.c keeps everything static next to its own main(), so instead of
 * calling its functions, each test runs the real ./client program and
 * surrounds it:
 *
 *            stdin pipe                    TCP (127.0.0.1)
 *   test  ------------->   ./client   <------------------->  fake server
 *   (types like a user)       |                              (this test)
 *                             | stdout pipe
 *   test  <-------------------+
 *   (reads what the user would see)
 *
 * The test types into the client, checks the JSON requests the fake server
 * receives, sends server lines back, and checks what the client prints.
 *
 * CLIENT_BIN (path to the client executable) is set by the Makefile.
 */

/* See worker_helpers.c: macOS <sys/socket.h> defines MSG_SEND as a macro. */
#include <sys/socket.h>
#undef MSG_SEND

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "framework.h"
#include "worker_helpers.h" /* recv_line, is_silent, json_*, sleep_ms */

/* The Makefile passes the client's absolute path. This fallback only keeps
   editors happy; tests run in temp directories, so build with `make test`. */
#ifndef CLIENT_BIN
#define CLIENT_BIN "build/client"
#endif

/* ======================================================================
 * Harness: a fake server plus a running client
 * ====================================================================== */

typedef struct {
    pid_t pid;
    int input_fd;  /* write here = the user typing */
    int output_fd; /* read here  = what the client prints */
    int conn_fd;   /* the fake server's end of the client's TCP connection */
} session_t;

/* Listen on 127.0.0.1 with a port chosen by the OS. */
static int listen_any_port(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(fd, 1) == 0);
    socklen_t len = sizeof(addr);
    CHECK(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Run ./client with the given arguments, wiring its stdin/stdout to pipes. */
static pid_t spawn_client(char *const argv[], int *input_fd, int *output_fd) {
    int in[2], out[2];
    CHECK(pipe(in) == 0 && pipe(out) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        close(in[0]); close(in[1]);
        close(out[0]); close(out[1]);
        execv(CLIENT_BIN, argv);
        perror("execv " CLIENT_BIN);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    *input_fd = in[1];
    *output_fd = out[0];
    return pid;
}

/* Start a fake server, launch the client against it, and accept its connection. */
static session_t start_session(const char *user, const char *conv) {
    int port;
    int listen_fd = listen_any_port(&port);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    char *argv[] = { "client", (char *)user, (char *)conv, "127.0.0.1", port_str, NULL };
    session_t s;
    s.pid = spawn_client(argv, &s.input_fd, &s.output_fd);

    struct pollfd p = { .fd = listen_fd, .events = POLLIN };
    CHECK(poll(&p, 1, WAIT_MS) == 1); /* client connected in time */
    s.conn_fd = accept(listen_fd, NULL, NULL);
    CHECK(s.conn_fd >= 0);
    close(listen_fd);
    return s;
}

/* The user types a line (a trailing newline is added). */
static void type_line(const session_t *s, const char *text) {
    size_t len = strlen(text);
    CHECK(write(s->input_fd, text, len) == (ssize_t)len);
    CHECK(write(s->input_fd, "\n", 1) == 1);
}

/* Write raw bytes from the fake server to the client. */
static void server_send(const session_t *s, const char *data) {
    size_t len = strlen(data);
    CHECK(write(s->conn_fd, data, len) == (ssize_t)len);
}

/* Read the next request the client sent to the server. */
static void server_recv(const session_t *s, char *line, size_t size) {
    CHECK(recv_line(s->conn_fd, line, size, WAIT_MS) > 0);
}

/* Read client output until a line containing `text` appears. */
static int output_contains(const session_t *s, const char *text) {
    char line[BUF_SIZE];
    while (recv_line(s->output_fd, line, sizeof(line), WAIT_MS) > 0) {
        if (strstr(line, text) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Wait for the client to exit; returns its wait status, or -1 on timeout. */
static int wait_exit(pid_t pid, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return status;
        }
        sleep_ms(10);
    }
    return -1;
}

static int exited_ok(int status) {
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/*
 * KNOWN RACE (not tested automatically, because it's timing-dependent):
 * on /quit or Ctrl-D, main() calls close(sockfd) while the receive thread is
 * still blocked in read() on that socket. On macOS the read() sometimes
 * returns EBADF first, so the client prints "read: Bad file descriptor" and
 * exits with status 1 (about 1 run in 100). Linux doesn't wake a blocked
 * read() on close(), so it rarely shows there. A test for it would pass or
 * fail at random, so the quit tests below only check that the client exits,
 * not its exit status.
 * Suggested fix: set a "quitting" flag before closing, and have the receive
 * thread exit quietly with status 0 when read() fails and the flag is set.
 */
static int exited(int status) {
    return status != -1 && (WIFEXITED(status) || WIFSIGNALED(status));
}

static void end_session(session_t *s) {
    kill(s->pid, SIGKILL); /* no-op if it already exited */
    waitpid(s->pid, NULL, 0);
    close(s->input_fd);
    close(s->output_fd);
    close(s->conn_fd);
}

/* Read the JOIN the client sends on connect (most tests don't care about it). */
static void skip_join(const session_t *s) {
    char line[BUF_SIZE];
    server_recv(s, line, sizeof(line));
    CHECK(json_field_is(line, "type", "JOIN"));
}

/* ======================================================================
 * Sending: what the server receives
 * ====================================================================== */

static void test_join_is_sent_on_connect(void) {
    session_t s = start_session("sam", "01");

    char line[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_field_is(line, "type", "JOIN"));
    CHECK(json_field_is(line, "user", "sam"));
    CHECK(json_field_is(line, "conversation_id", "01"));
    CHECK(strstr(line, "\"message\":null") != NULL);

    end_session(&s);
}

static void test_typed_message_is_sent(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    type_line(&s, "hello world");

    char line[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_field_is(line, "type", "SEND"));
    CHECK(json_field_is(line, "user", "sam"));
    CHECK(json_field_is(line, "conversation_id", "01"));
    CHECK(json_field_is(line, "message", "hello world"));

    end_session(&s);
}

static void test_quotes_and_backslashes_are_escaped(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    type_line(&s, "she said \"hi\" \\o/");

    char line[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_field_is(line, "message", "she said \"hi\" \\o/"));

    end_session(&s);
}

static void test_empty_line_is_not_sent(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    type_line(&s, "");
    type_line(&s, "real");

    char line[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_field_is(line, "message", "real")); /* the empty line was skipped */

    end_session(&s);
}

static void test_messages_are_sent_in_order(void) {
    enum { COUNT = 20 };
    session_t s = start_session("sam", "01");
    skip_join(&s);

    for (int i = 0; i < COUNT; i++) {
        char msg[16];
        snprintf(msg, sizeof(msg), "msg-%d", i);
        type_line(&s, msg);
    }
    for (int i = 0; i < COUNT; i++) {
        char line[BUF_SIZE], expected[16];
        snprintf(expected, sizeof(expected), "msg-%d", i);
        server_recv(&s, line, sizeof(line));
        CHECK(json_field_is(line, "message", expected));
    }

    end_session(&s);
}

static void test_quit_sends_leave_and_exits(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    type_line(&s, "/quit");

    char line[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_field_is(line, "type", "LEAVE"));
    CHECK(json_field_is(line, "user", "sam"));
    CHECK(exited(wait_exit(s.pid, WAIT_MS))); /* see KNOWN RACE above */

    end_session(&s);
}

static void test_end_of_input_sends_leave_and_exits(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    close(s.input_fd); /* like pressing Ctrl-D */
    s.input_fd = -1;

    char line[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_field_is(line, "type", "LEAVE"));
    CHECK(exited(wait_exit(s.pid, WAIT_MS))); /* see KNOWN RACE above */

    end_session(&s);
}

/* ======================================================================
 * Receiving: what the user sees
 * ====================================================================== */

static void test_prints_broadcast_message(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    server_send(&s, "{\"sender\":\"alice\", \"message\":\"hi sam\"}\n");
    CHECK(output_contains(&s, "alice: hi sam"));

    end_session(&s);
}

/* History lines (sent on JOIN) use a different format, with a timestamp. */
static void test_prints_history_message(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    server_send(&s, "{\"timestamp\": 1730000000, \"sender\": \"alice\", \"message\": \"earlier\"}\n");
    CHECK(output_contains(&s, "alice: earlier"));

    end_session(&s);
}

static void test_prints_unescaped_quotes(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    server_send(&s, "{\"sender\":\"bob\", \"message\":\"say \\\"hi\\\"\"}\n");
    CHECK(output_contains(&s, "bob: say \"hi\""));

    end_session(&s);
}

/* TCP is a byte stream: one message can arrive in several pieces. */
static void test_line_split_across_reads_is_printed_once(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    server_send(&s, "{\"sender\":\"alice\", \"mess");
    sleep_ms(100); /* make sure the client reads the first half on its own */
    server_send(&s, "age\":\"in two parts\"}\n");
    CHECK(output_contains(&s, "alice: in two parts"));

    end_session(&s);
}

static void test_several_lines_in_one_read_are_all_printed(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    server_send(&s,
                "{\"sender\":\"a\", \"message\":\"one\"}\n"
                "{\"sender\":\"b\", \"message\":\"two\"}\n"
                "{\"sender\":\"c\", \"message\":\"three\"}\n");
    CHECK(output_contains(&s, "a: one"));
    CHECK(output_contains(&s, "b: two"));
    CHECK(output_contains(&s, "c: three"));

    end_session(&s);
}

static void test_unparseable_line_is_printed_raw(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    server_send(&s, "server is restarting\n");
    CHECK(output_contains(&s, "server is restarting"));

    end_session(&s);
}

/*
 * The reason the client has two threads: a message that arrives while the
 * user is halfway through typing must still be shown immediately, and the
 * half-typed message must still be sent once they press Enter.
 */
static void test_message_arrives_while_user_is_typing(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    CHECK(write(s.input_fd, "half-typ", 8) == 8); /* no Enter yet */
    server_send(&s, "{\"sender\":\"alice\", \"message\":\"interrupting\"}\n");
    CHECK(output_contains(&s, "alice: interrupting"));

    type_line(&s, "ed message");
    char line[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_field_is(line, "message", "half-typed message"));

    end_session(&s);
}

static void test_server_disconnect_exits_cleanly(void) {
    session_t s = start_session("sam", "01");
    skip_join(&s);

    close(s.conn_fd); /* server goes away */
    s.conn_fd = -1;
    CHECK(output_contains(&s, "server closed the connection"));
    CHECK(exited_ok(wait_exit(s.pid, WAIT_MS)));

    end_session(&s);
}

/* ======================================================================
 * Startup errors
 * ====================================================================== */

static void test_missing_arguments_prints_usage(void) {
    int input_fd, output_fd;
    char *argv[] = { "client", "sam", NULL };
    pid_t pid = spawn_client(argv, &input_fd, &output_fd);

    int status = wait_exit(pid, WAIT_MS);
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

static void test_no_server_fails_to_connect(void) {
    int port;
    close(listen_any_port(&port)); /* a port that nothing is listening on */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int input_fd, output_fd;
    char *argv[] = { "client", "sam", "01", "127.0.0.1", port_str, NULL };
    pid_t pid = spawn_client(argv, &input_fd, &output_fd);

    int status = wait_exit(pid, WAIT_MS);
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

/* ======================================================================
 * Known bugs (XFAIL)
 * ====================================================================== */

/* Only the message is escaped; the username and conversation id are not. */
static void test_username_with_quote_is_escaped(void) {
    session_t s = start_session("sa\"m", "01");

    char line[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_field_is(line, "user", "sa\"m"));

    end_session(&s);
}

/* fgets reads at most 1023 characters, so longer input is split up. */
static void test_long_message_is_sent_as_one_message(void) {
    enum { LEN = 2000 };
    static char big[LEN + 1];
    memset(big, 'x', LEN);
    big[LEN] = '\0';

    session_t s = start_session("sam", "01");
    skip_join(&s);

    type_line(&s, big);

    char line[BUF_SIZE], msg[BUF_SIZE];
    server_recv(&s, line, sizeof(line));
    CHECK(json_get_string(line, "message", msg, sizeof(msg)));
    CHECK(strlen(msg) == LEN);

    end_session(&s);
}

/* ====================================================================== */

static const test_case_t tests[] = {
    /* sending */
    TEST(test_join_is_sent_on_connect),
    TEST(test_typed_message_is_sent),
    TEST(test_quotes_and_backslashes_are_escaped),
    TEST(test_empty_line_is_not_sent),
    TEST(test_messages_are_sent_in_order),
    TEST(test_quit_sends_leave_and_exits),
    TEST(test_end_of_input_sends_leave_and_exits),
    /* receiving */
    TEST(test_prints_broadcast_message),
    TEST(test_prints_history_message),
    TEST(test_prints_unescaped_quotes),
    TEST(test_line_split_across_reads_is_printed_once),
    TEST(test_several_lines_in_one_read_are_all_printed),
    TEST(test_unparseable_line_is_printed_raw),
    TEST(test_message_arrives_while_user_is_typing),
    TEST(test_server_disconnect_exits_cleanly),
    /* startup errors */
    TEST(test_missing_arguments_prints_usage),
    TEST(test_no_server_fails_to_connect),
    /* known bugs */
    TEST_XFAIL(test_username_with_quote_is_escaped,
               "username and conversation id are not JSON-escaped (send_request)"),
    TEST_XFAIL(test_long_message_is_sent_as_one_message,
               "input over 1023 chars is split into several messages (fgets buffer in main)"),
};

int main(int argc, char **argv) {
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]), argc, argv);
}
