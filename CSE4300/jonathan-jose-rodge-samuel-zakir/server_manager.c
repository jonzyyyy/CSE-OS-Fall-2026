#include "server_manager.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

//linked list node where each node stores a pointer to one conversations worker and pointer to next node
typedef struct Conversation{
    worker_t *worker;
    struct Conversation *next;
}Conversation;

//managers state- conversations points to first node in registry and a lock protects access to registry
struct ServerManager{
    Conversation *conversations;
    pthread_mutex_t lock; //using lock for critical section
    int stopping;//records whether shutdown has started
};

//Allocating an independent copy of the string-this will give queued requests their own strings
static char *copy_string(const char *source){
    char *copy = malloc(strlen(source)+1);//allocating space for string
    
    if(copy != NULL){
        strcpy(copy, source);
    }
    return copy;
}

/*calling while manager lock is held
    this  will find an existing conversation*/
static worker_t *find_worker(ServerManager *manager, const char *conversation_id)
{
    Conversation *entry;
    //loop will walk through linked list comparing ids to find right conversation
    for(entry = manager->conversations; entry!=NULL; entry = entry->next){
        if(strcmp(entry->worker->conv_id, conversation_id) == 0){//when strings match it will return 0, locating correct convo_id
            return entry->worker;
        }
    }
    return NULL;
}

//start and register a new conversation worker using worker_create function and inserting it into Conversation
static worker_t *create_worker(ServerManager *manager, const char *conversation_id){
    Conversation *entry = malloc(sizeof(*entry));
    if(entry == NULL){
        return NULL;
    }

    entry->worker = worker_create(conversation_id);

    if(entry->worker == NULL){
        free(entry);
        return NULL;
    }
    entry->next = manager->conversations;
    manager->conversations = entry;

    return entry->worker;
}
/*free before transferring ownership to worker*/
static void free_item(queue_item_t *item){
    free(item->user);
    free(item->msg);
    free(item);
}

int manager_init(ServerManager **out){
    ServerManager *manager;
    int error;

    if(out == NULL){
        return EINVAL;
    }
    *out = NULL;

    manager = calloc(1, sizeof(*manager));
    if(manager == NULL){
        return ENOMEM;
    }

    error = pthread_mutex_init(&manager->lock, NULL);

    if(error != 0){
        free(manager);
        return error;
    }
 
    *out = manager;
    return 0;
}

int manager_handle_request(
    ServerManager *manager,
    const char *conversation_id,
    msg_type_t type,
    const char *user,
    const char *message,
    int client_fd
)
{
    queue_item_t *item;
    worker_t *worker;
    int error;

    //validate the required fields//
    if(
        manager == NULL ||
        conversation_id == NULL ||
        conversation_id[0] == '\0' ||
        user == NULL ||
        user[0] == '\0' ||
        client_fd <0
    ){
        return EINVAL;
    }

    if(type != MSG_SEND && type != MSG_JOIN && type != MSG_LEAVE){
        return EINVAL;
    }

    if(type == MSG_SEND && message == NULL){
        return EINVAL;
    }

    /*worker_t stores conversation ids in char conv_id[64] and rejects ids that can be truncated*/
    if(strlen(conversation_id)>= sizeof((worker_t *)0)->conv_id){
        return ENAMETOOLONG;
    }
    /*Allocate the queue item and copy its strings.*/
    item = calloc(1, sizeof(*item));

    if (item == NULL){
        return ENOMEM;
    }
    item->type = type;
    item->client_fd = client_fd;
    item->user = copy_string(user);

    if(message != NULL){
        item->msg = copy_string(message);
    }
    if(item->user ==NULL || (message != NULL && item->msg == NULL)){
        free_item(item);
        return ENOMEM;
    }

    error = pthread_mutex_lock(&manager->lock);
    if(error != 0){
        free_item(item);
        return error;
    }
    if(manager->stopping){
        pthread_mutex_unlock(&manager->lock);
        free_item(item);
        return ECANCELED;
    }

    worker = find_worker(manager, conversation_id);

    /*if there is no conversation then create a new worker for it*/
    if(worker == NULL){
        worker = create_worker(manager, conversation_id);
    }
    if(worker == NULL){
        pthread_mutex_unlock(&manager->lock);
        free_item(item);
        /*since worker_create() returns NULL without an error code the manager will report a general creation failure*/
        return EAGAIN;
    }
    /*transfer ownership to the worker, the worker will free item, item->user, and item->msg*/
    worker_enqueue(worker, item);
    pthread_mutex_unlock(&manager->lock);
    return 0;
}

int manager_shutdown(ServerManager **handle){
    ServerManager *manager;
    Conversation *entry;
    int error;

    if (handle == NULL){
        return EINVAL;
    }
    manager=*handle;

    if(manager == NULL){
        return 0;
    }

    error = pthread_mutex_lock(&manager->lock);

    if(error!=0){
        return error;
    }
    manager->stopping = 1;

    //signal every worker and waiting for any to finish//
    for(entry = manager->conversations;entry != NULL;entry = entry->next){
        worker_t *worker = entry->worker;
        error = pthread_mutex_lock(&worker->lock);
        if(error != 0){
            pthread_mutex_unlock(&manager->lock);
            return error;
        }

        worker->running =0;
        //wake up workers waiting on an empty queue//
        pthread_cond_signal(&worker->not_empty);
        pthread_mutex_unlock(&worker->lock);
    }
    pthread_mutex_unlock(&manager->lock);

    //join workers and release resources//
    while(manager->conversations != NULL){
        worker_t *worker;
        entry = manager->conversations;
        worker = entry->worker;
        error = pthread_join(worker->tid, NULL);

        if(error!=0){
            return error;
        }
        manager->conversations = entry->next;

        pthread_cond_destroy(&worker->not_empty);
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        free(entry);
    }

    error = pthread_mutex_destroy(&manager->lock);
    if(error!=0){
        return error;
    }
    free(manager);
    *handle = NULL;
    return 0;
}
