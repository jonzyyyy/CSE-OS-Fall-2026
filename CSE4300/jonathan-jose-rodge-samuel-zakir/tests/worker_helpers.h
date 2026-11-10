/*
 * worker_helpers.h - helpers for testing the worker thread in isolation.
 *
 * The worker only ever talks to clients through file descriptors, so tests
 * stand in for real TCP clients with socketpair(): the worker writes to one
 * end and the test reads what a client would have received from the other.
 * The test also plays the manager's role by enqueueing messages directly.
 */
#ifndef WORKER_HELPERS_H
#define WORKER_HELPERS_H

#include <stddef.h>

#include "server_worker.h"

/* How long to wait for something that SHOULD arrive. */
#define WAIT_MS 2000
/* How long to wait before concluding that nothing WILL arrive. */
#define QUIET_MS 150

#define BUF_SIZE 16384

/* A fake client connection. */
typedef struct {
    int server_fd; /* the end the worker writes to (what the server holds) */
    int client_fd; /* the end the test reads from (what the client holds)  */
} fake_client_t;

fake_client_t fake_client_new(void);
void fake_client_disconnect(fake_client_t *c); /* close the client's end */

/* Enqueue a message onto the worker, as the manager would. msg may be NULL. */
void enqueue(worker_t *w, msg_type_t type, int fd, const char *user, const char *msg);
void client_join(worker_t *w, const fake_client_t *c, const char *user);
void client_send(worker_t *w, const fake_client_t *c, const char *user, const char *msg);
void client_leave(worker_t *w, const fake_client_t *c, const char *user);

/*
 * Read one '\n'-terminated line. Returns its length, or -1 on timeout,
 * disconnect, or if the line does not fit in buf.
 */
int recv_line(int fd, char *buf, size_t size, int timeout_ms);
/* Returns 1 if nothing arrives on fd within timeout_ms. */
int is_silent(int fd, int timeout_ms);

/*
 * Extract the string value of "key" from a single-line JSON object,
 * decoding standard escapes. Returns 1 on success, 0 if missing/malformed.
 * Deliberately simple: good enough for the flat objects the server emits.
 */
int json_get_string(const char *line, const char *key, char *out, size_t size);
/* Returns 1 if "key" exists and its string value equals expected. */
int json_field_is(const char *line, const char *key, const char *expected);

/* Path of the worker's conversation history file. */
void history_path(const char *conv_id, char *buf, size_t size);
int history_line_count(const char *conv_id);
/* Poll until the history has at least expected lines; returns the final count. */
int wait_for_history_lines(const char *conv_id, int expected, int timeout_ms);
/* Copy line number index (0-based) of the history into buf. Returns 1 if found. */
int read_history_line(const char *conv_id, int index, char *buf, size_t size);

/*
 * Stop the worker thread, wait for it to drain its queue, and free it.
 * server_worker.c has no shutdown function yet, so this does what one would:
 * clear `running`, wake the thread, and join it.
 */
void worker_stop(worker_t *w);

void sleep_ms(int ms);

#endif
