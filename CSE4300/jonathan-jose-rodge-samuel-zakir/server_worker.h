#ifndef SERVER_WORKER_H
#define SERVER_WORKER_H

#include <pthread.h>

#define MAX_CLIENTS 32

typedef enum {MSG_SEND, MSG_JOIN, MSG_LEAVE} msg_type_t;

typedef struct queue_item{
    msg_type_t type;
    int client_fd;
    char *user;
    char *msg;
    struct queue_item *next;

}queue_item_t;

typedef struct worker {
    char conv_id[64];
    pthread_t tid;
    queue_item_t *head, *tail;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    int running;
    int clients[MAX_CLIENTS];
    int num_clients;
}worker_t;

worker_t *worker_create(const char *conv_id);
void worker_enqueue(worker_t *w, queue_item_t *item);

#endif