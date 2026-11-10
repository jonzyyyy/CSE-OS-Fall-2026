/*
 * WORKAROUND: macOS <sys/socket.h> defines a macro MSG_SEND (0x1000), which
 * clashes with MSG_SEND in server_worker.h's msg_type_t enum. Without the
 * #undef, every MSG_SEND enqueued from this file has type 0x1000 and the
 * worker silently drops it. Remove once the enum values are renamed
 * (e.g. MSG_TYPE_SEND); the manager will hit the same problem.
 */
#include <sys/socket.h>
#undef MSG_SEND

#include "worker_helpers.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "framework.h"

/* ---- fake clients ------------------------------------------------------ */

fake_client_t fake_client_new(void) {
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fake_client_t c = { .server_fd = fds[0], .client_fd = fds[1] };
    return c;
}

void fake_client_disconnect(fake_client_t *c) {
    close(c->client_fd);
    c->client_fd = -1;
}

/* ---- enqueueing (playing the manager) ---------------------------------- */

void enqueue(worker_t *w, msg_type_t type, int fd, const char *user, const char *msg) {
    queue_item_t *item = calloc(1, sizeof(*item));
    CHECK(item != NULL);
    item->type = type;
    item->client_fd = fd;
    item->user = user ? strdup(user) : NULL;
    item->msg = msg ? strdup(msg) : NULL;
    worker_enqueue(w, item); /* the worker takes ownership and frees it */
}

void client_join(worker_t *w, const fake_client_t *c, const char *user) {
    enqueue(w, MSG_JOIN, c->server_fd, user, NULL);
}

void client_send(worker_t *w, const fake_client_t *c, const char *user, const char *msg) {
    enqueue(w, MSG_SEND, c->server_fd, user, msg);
}

void client_leave(worker_t *w, const fake_client_t *c, const char *user) {
    enqueue(w, MSG_LEAVE, c->server_fd, user, NULL);
}

/* ---- reading what clients receive -------------------------------------- */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int recv_line(int fd, char *buf, size_t size, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    size_t len = 0;

    while (len + 1 < size) {
        long remaining = deadline - now_ms();
        if (remaining <= 0) {
            break;
        }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, (int)remaining) <= 0) {
            break;
        }
        char ch;
        if (read(fd, &ch, 1) != 1) {
            break; /* disconnected */
        }
        buf[len++] = ch;
        if (ch == '\n') {
            buf[len] = '\0';
            return (int)len;
        }
    }
    buf[len] = '\0';
    return -1;
}

int is_silent(int fd, int timeout_ms) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, timeout_ms) == 0;
}

/* ---- JSON ---------------------------------------------------------------- */

int json_get_string(const char *line, const char *key, char *out, size_t size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(line, pattern);
    if (p == NULL) {
        return 0;
    }
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ') p++;
    if (*p++ != '"') return 0;

    size_t len = 0;
    while (*p != '\0' && *p != '"') {
        char ch = *p++;
        if (ch == '\\') {
            switch (*p) {
            case 'n':  ch = '\n'; break;
            case 't':  ch = '\t'; break;
            case '"':
            case '\\':
            case '/':  ch = *p;   break;
            default:   return 0;
            }
            p++;
        }
        if (len + 1 >= size) {
            return 0;
        }
        out[len++] = ch;
    }
    if (*p != '"') {
        return 0; /* unterminated string */
    }
    out[len] = '\0';
    return 1;
}

int json_field_is(const char *line, const char *key, const char *expected) {
    char value[BUF_SIZE];
    return json_get_string(line, key, value, sizeof(value)) && strcmp(value, expected) == 0;
}

/* ---- conversation history file ----------------------------------------- */

void history_path(const char *conv_id, char *buf, size_t size) {
    /* Must match the naming used in server_worker.c. */
    snprintf(buf, size, "conversation_%s.jsonl", conv_id);
}

int history_line_count(const char *conv_id) {
    char path[256];
    history_path(conv_id, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    int lines = 0, ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') lines++;
    }
    fclose(f);
    return lines;
}

int wait_for_history_lines(const char *conv_id, int expected, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    int lines = history_line_count(conv_id);
    while (lines < expected && now_ms() < deadline) {
        sleep_ms(5);
        lines = history_line_count(conv_id);
    }
    return lines;
}

int read_history_line(const char *conv_id, int index, char *buf, size_t size) {
    char path[256];
    history_path(conv_id, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    int found = 0;
    for (int i = 0; fgets(buf, (int)size, f) != NULL; i++) {
        if (i == index) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* ---- worker lifecycle -------------------------------------------------- */

void worker_stop(worker_t *w) {
    pthread_mutex_lock(&w->lock);
    w->running = 0;
    pthread_cond_broadcast(&w->not_empty);
    pthread_mutex_unlock(&w->lock);

    pthread_join(w->tid, NULL);
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->not_empty);
    free(w);
}
