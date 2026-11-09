#ifndef SERVER_MANAGER_H
#define SERVER_MANAGER_H

#include "server_worker.h"

typedef struct ServerManager ServerManager;

/*returns 0 for success in creating manager*/
int manager_init(ServerManager **out);

/*routing a parsed request to the correct worker
    Input strings get copied before this function returns*/
int manager_handle_request(
    ServerManager *manager,
    const char *conversation_id,
    msg_type_t type, 
    const char *user,
    const char *message,
    int client_fd
);

/*Call after all incoming request producers have stopped
    this will stop join workers and release their resources*/

int manager_shutdown(ServerManager **handle);




#endif