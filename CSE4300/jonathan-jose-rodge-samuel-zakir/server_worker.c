#include "server_worker.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

static queue_item_t *queue_pop(worker_t *w){
    queue_item_t *head = w->head;
    if(head == NULL) return NULL;
    w->head = head->next;
    if (w->head == NULL)
    {
        w->tail = NULL;
    }
    return head;
}
static int find_client(worker_t *w, int fd){
    for (int i = 0; i < w->num_clients; i++)
    {
        if (w->clients[i] == fd)
        {
            return i;
        }   
    }
    return -1;
}

static int add_client(worker_t *w, int fd){
    if (find_client(w, fd)!= -1)
    {
        return 0;
    }
    if (w->num_clients == MAX_CLIENTS)
    {
        return -1;
    }
    w->clients[w->num_clients] = fd;
    w->num_clients++;
    return 0;
}
static void remove_client(worker_t *w, int fd){
    int i = find_client(w, fd);
    if (i == -1) return;
    w->clients[i] = w->clients[w->num_clients-1];
    w->num_clients --;
}
static int write_all(int fd, const char *buf, size_t len){
    size_t sent = 0;
    while(sent < len){
        ssize_t n = write(fd, buf+sent, len-sent);
        if(n<=0) return -1;
        sent +=n;
    }
    return 0;
}
static void handle_send(worker_t *w, queue_item_t *item){
    int sender = item->client_fd;
    if(item->msg == NULL)return;

    add_client(w, sender);

    char line[4096];
    snprintf(line, sizeof(line), "{\"timestamp\": %ld, \"sender\": \"%s\", \"message\": \"%s\"}\n", (long)time(NULL), item->user, item->msg);

    char path[128];
    snprintf(path, sizeof(path), "conversation_%s.jsonl", w->conv_id);
    FILE *f = fopen(path, "a");
    if (f != NULL)
    {
        fputs(line, f);
        fclose(f);
    }
    snprintf(line, sizeof(line), "{\"sender\":\"%s\", \"message\":\"%s\"}\n", item->user, item->msg);

    for (int i = w->num_clients-1; i >=0; i--)
    {
        int fd = w->clients[i];
        if(fd == sender) continue;
        if(write_all(fd, line, strlen(line)) == -1){
            remove_client(w, fd);
        }
    }
    
}
static void handle_leave(worker_t *w, queue_item_t *item){
    remove_client(w, item->client_fd);
}
static void handle_join(worker_t *w, queue_item_t *item){
    int fd = item->client_fd;

    if (add_client(w, fd) == -1) return;
    char path[128];
    snprintf(path, sizeof(path), "conversation_%s.jsonl", w->conv_id);
    FILE *f = fopen(path, "r");
    if(f==NULL) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)!=NULL)
    {
        write_all(fd, line, strlen(line));
    }
    fclose(f);
}
static void *worker_main(void *arg) {
    worker_t *w = arg;
    while(1){
        pthread_mutex_lock(&w->lock);
        while(w->head == NULL && w->running){
            pthread_cond_wait(&w->not_empty, &w->lock);
        }

        if(!w->running && w->head == NULL){
            pthread_mutex_unlock(&w->lock);
            break;
        }
        
        queue_item_t *item = queue_pop(w);
        pthread_mutex_unlock(&w->lock);
    
        switch (item->type)
        {
        case (MSG_SEND):
            handle_send(w, item);
            break;
        
        case (MSG_LEAVE):
            handle_leave(w, item);
            break;
        
        case (MSG_JOIN):
            handle_join(w, item);
            break;
        }
        free(item->user);
        free(item->msg);
        free(item);
    }
    
    return NULL;
}
worker_t *worker_create(const char *conv_id){
    worker_t *w = calloc(1, sizeof(worker_t));
    if (w == NULL) return NULL;
    strncpy(w->conv_id, conv_id, sizeof(w->conv_id)-1);
    w->running = 1;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->not_empty, NULL);

    if (pthread_create(&w->tid, NULL, worker_main, w)!=0)
    {
        pthread_mutex_destroy(&w->lock);
        pthread_cond_destroy(&w->not_empty);
        free(w);
        return NULL;
    }
    return w;
}
void worker_enqueue(worker_t *w, queue_item_t *item){
    pthread_mutex_lock(&w->lock);
    item->next = NULL;

    if (w->tail == NULL){
        w->head = item;
        w->tail = item;
    }
    else {
        w->tail->next = item;
        w->tail = item;
    }

    pthread_cond_signal(&w->not_empty);
    pthread_mutex_unlock(&w->lock);
}
