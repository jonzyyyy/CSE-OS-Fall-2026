/*
 * server.c - CSE 4300 multithreaded chat server (network front end)
 *
 * Connects the client to the manager:
 *
 *   client --TCP JSON lines--> connection thread --> manager --> worker
 *   client <------------- worker writes directly to the socket --------
 *
 *   main thread       : listens on the port and accepts connections
 *   connection thread : one per client; reads JSON request lines, parses
 *                       them, and hands each one to manager_handle_request()
 *
 * Replies never pass through the connection thread: the worker for the
 * conversation writes history and broadcasts straight to the client's fd.
 *
 * Usage:   ./server [port]        (default 8080; 0 = let the OS pick)
 * Prints "listening on port N" once it is ready for clients.
 */

/* macOS <sys/socket.h> defines MSG_SEND as a macro, which clashes with
   msg_type_t in server_worker.h. Include it first and drop the macro. */
#include <sys/socket.h>
#undef MSG_SEND

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server_manager.h"

#define DEFAULT_PORT 8080
#define LINE_MAX_LEN 4096
#define FIELD_MAX    2048
#define MAX_JOINED   16   /* conversations one connection can be in at once */

/* Created once in main() before any connection thread starts.
   manager_handle_request() does its own locking. */
static ServerManager *manager;

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

/* Find "key" in a JSON line and copy its string value into out, KEEPING
 * escape sequences as they are (\" stays \"). The worker puts the text
 * into its own JSON lines without escaping, so passing it through still
 * escaped keeps those lines valid; the client unescapes when it prints.
 *
 * Returns 1 if found, 0 if missing or not a string (e.g. "message":null),
 * -1 if the value does not fit in out. */
static int get_field_raw(const char *line, const char *key, char *out, size_t outsize)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(line, pattern);
    if (p == NULL)
        return 0;
    p += strlen(pattern);

    while (*p == ' ' || *p == ':')
        p++;
    if (*p != '"')
        return 0;
    p++;

    size_t j = 0;
    while (*p != '\0' && *p != '"') {
        size_t n = (*p == '\\' && p[1] != '\0') ? 2 : 1;  /* keep \x pairs together */
        if (j + n >= outsize)
            return -1;
        while (n--)
            out[j++] = *p++;
    }
    if (*p != '"')          /* line ended before the closing quote */
        return 0;
    out[j] = '\0';
    return 1;
}

static int parse_type(const char *s, msg_type_t *out)
{
    if (strcmp(s, "SEND") == 0)  { *out = MSG_SEND;  return 1; }
    if (strcmp(s, "JOIN") == 0)  { *out = MSG_JOIN;  return 1; }
    if (strcmp(s, "LEAVE") == 0) { *out = MSG_LEAVE; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-connection state                                                */
/* ------------------------------------------------------------------ */

/* Remembers which conversations this client joined, so they can all be
   left when the client disconnects without sending LEAVE. Only touched
   by its own connection thread, so no lock is needed. */
typedef struct {
    int fd;
    char user[64];
    char joined[MAX_JOINED][64];
    int num_joined;
} connection_t;

static int find_joined(const connection_t *c, const char *conv)
{
    for (int i = 0; i < c->num_joined; i++)
        if (strcmp(c->joined[i], conv) == 0)
            return i;
    return -1;
}

static void remember_join(connection_t *c, const char *conv)
{
    if (find_joined(c, conv) != -1 || c->num_joined == MAX_JOINED)
        return;
    snprintf(c->joined[c->num_joined++], sizeof(c->joined[0]), "%s", conv);
}

static void forget_join(connection_t *c, const char *conv)
{
    int i = find_joined(c, conv);
    if (i == -1)
        return;
    c->num_joined--;
    memcpy(c->joined[i], c->joined[c->num_joined], sizeof(c->joined[0]));
}

/* Parse one request line and pass it to the manager. Bad lines are
   reported on stderr and skipped; the connection stays open. */
static void handle_line(connection_t *c, const char *line)
{
    char conv[64], type_str[16], user[64], message[FIELD_MAX];
    msg_type_t type;

    if (get_field_raw(line, "conversation_id", conv, sizeof(conv)) != 1 ||
        get_field_raw(line, "type", type_str, sizeof(type_str)) != 1 ||
        get_field_raw(line, "user", user, sizeof(user)) != 1 ||
        !parse_type(type_str, &type)) {
        fprintf(stderr, "server: fd %d: ignoring bad request: %s\n", c->fd, line);
        return;
    }

    int has_msg = get_field_raw(line, "message", message, sizeof(message));
    if (has_msg == -1) {
        fprintf(stderr, "server: fd %d: message too long, ignored\n", c->fd);
        return;
    }

    int err = manager_handle_request(manager, conv, type, user,
                                     has_msg ? message : NULL, c->fd);
    if (err != 0) {
        fprintf(stderr, "server: fd %d: request rejected (%s)\n", c->fd, strerror(err));
        return;
    }

    snprintf(c->user, sizeof(c->user), "%s", user);
    if (type == MSG_JOIN)
        remember_join(c, conv);
    else if (type == MSG_LEAVE)
        forget_join(c, conv);
}

/* ------------------------------------------------------------------ */
/* Connection thread                                                   */
/* ------------------------------------------------------------------ */

/* Same buffering as the client's receive thread: TCP is a byte stream,
   so collect bytes and handle each complete '\n'-terminated line. */
static void *connection_thread(void *arg)
{
    connection_t *c = arg;
    char buf[LINE_MAX_LEN];
    size_t used = 0;

    for (;;) {
        ssize_t n = read(c->fd, buf + used, sizeof(buf) - used - 1);
        if (n <= 0)
            break;                              /* client disconnected */
        used += n;

        char *nl;
        while ((nl = memchr(buf, '\n', used)) != NULL) {
            *nl = '\0';
            handle_line(c, buf);

            size_t line_len = (nl - buf) + 1;
            memmove(buf, nl + 1, used - line_len);
            used -= line_len;
        }

        if (used == sizeof(buf) - 1) {          /* line too long: drop it */
            fprintf(stderr, "server: fd %d: request line too long, dropped\n", c->fd);
            used = 0;
        }
    }

    /* Leave anything the client didn't leave itself (crash, Ctrl-C, ...). */
    for (int i = 0; i < c->num_joined; i++)
        manager_handle_request(manager, c->joined[i], MSG_LEAVE,
                               c->user[0] ? c->user : "unknown", NULL, c->fd);

    /* KNOWN RACE: the LEAVEs above are queued, not processed yet. If a new
       client is accepted and gets this same fd number before a worker
       handles its LEAVE, that worker could briefly write to the new client.
       Fix later by letting the worker close the fd after the final LEAVE. */
    close(c->fd);
    free(c);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc > 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return 1;
    }
    int port = (argc == 2) ? atoi(argv[1]) : DEFAULT_PORT;

    /* Workers write to client sockets. Writing to a socket whose client
       has gone away raises SIGPIPE, which would kill the whole server.
       Ignore it so write() just fails and the worker drops that client. */
    signal(SIGPIPE, SIG_IGN);

    if (manager_init(&manager) != 0) {
        fprintf(stderr, "server: failed to create manager\n");
        return 1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return 1;
    }
    int yes = 1;    /* allow restarting right away on the same port */
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 16) != 0) {
        perror("listen");
        return 1;
    }

    socklen_t len = sizeof(addr);       /* find the real port if 0 was given */
    getsockname(listen_fd, (struct sockaddr *)&addr, &len);
    printf("listening on port %d\n", ntohs(addr.sin_port));
    fflush(stdout);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            perror("accept");
            continue;
        }

        connection_t *c = calloc(1, sizeof(*c));
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->fd = fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, connection_thread, c) != 0) {
            fprintf(stderr, "server: failed to start connection thread\n");
            close(fd);
            free(c);
            continue;
        }
        pthread_detach(tid);
    }
}
