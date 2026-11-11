/*
 * client.c - CSE 4300 multithreaded chat client
 *
 * Built on the GeeksforGeeks TCP client. The socket()/connect() code in
 * main() is theirs, unchanged. Their func() ping-pong loop is gone: it did
 * write-then-read in lockstep, so you could never receive a message while
 * typing. It's replaced by two threads:
 *
 *   main thread    : reads what the user types -> sends JSON to the server
 *   receive thread : reads what the server sends -> prints it
 *
 * Usage:   ./client <username> <conversation_id>
 * Test without the server:   terminal 1: nc -l 8080
 *                            terminal 2: ./client sam 01
 *
 * Work through the TODOs in order (STEP 1, 2, ...). Each step should
 * compile and run on its own, so test after every one.
 */

#include <arpa/inet.h>   // inet_addr()
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     // bzero()
#include <sys/socket.h>
#include <unistd.h>      // read(), write(), close()

#define MAX  1024        // was 80 in G4G; chat lines can be longer
#define PORT 8080        // TODO: confirm with whoever writes the server main()
#define SA struct sockaddr

/* Shared between both threads. Set once in main() before the receive
 * thread starts and never changed after, so no lock is needed. */
static int  sockfd;
static char username[64];
static char conv_id[64];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* write() can send fewer bytes than you asked for. This loops until the
 * whole buffer is out. Same idea as write_all() in server_worker.c.
 * (Filled in for you: it's boilerplate, not the interesting part.) */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0)
            return -1;
        sent += n;
    }
    return 0;
}

/*
 * STEP 2: build one request line and send it.
 *
 * Should produce exactly one line, ending in '\n', like:
 *   {"conversation_id":"01","type":"SEND","user":"sam","message":"hello"}\n
 * For JOIN and LEAVE, message is NULL -> write  "message":null  (no quotes).
 *
 * Hints:
 *   - char line[MAX * 2];  then snprintf(line, sizeof(line), "...", ...)
 *   - Inside a C string, a literal quote is \"
 *   - Two snprintf calls (one for message == NULL, one for not) is fine.
 *   - Send it with write_all(sockfd, line, strlen(line)).
 *
 * Return 0 on success, -1 if write_all fails.
 */
static int send_request(const char *type, const char *message)
{
    char line[MAX * 2];

    /* TODO STEP 2: build the JSON line with snprintf */
    (void)type;
    (void)message;
    line[0] = '\0';
    snprintf(line, sizeof(line),
         "{\"conversation_id\":\"%s\",\"type\":\"%s\",\"user\":\"%s\",\"message\":\"%s\"}\n",
         conv_id, type, username, message);
    /* TODO STEP 2: send it */

    (void)line;
    return 0;
}

/*
 * STEP 5 (do this last): turn a raw server line into "sender: message".
 *
 * The server sends two shapes:
 *   history (after JOIN): {"timestamp": 1730000000, "sender": "Alice", "message": "Hello"}
 *   live messages:        {"sender":"Alice", "message":"Hello"}
 * Note the spacing differs, so don't match on exact spacing.
 *
 * Hint: strstr(line, "\"sender\"") finds the key. From there, skip to the
 * next '"' after the ':', and copy characters until the closing '"'.
 * Writing one helper  get_field(line, "sender", out, outsize)  and calling
 * it twice is the clean way.
 *
 * Until you do this step, just print the raw line.
 */
static void print_server_line(const char *line)
{
    /* TODO STEP 5: parse sender + message and print "sender: message" */
    printf("%s\n", line);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Receive thread                                                      */
/* ------------------------------------------------------------------ */

/*
 * STEP 4: loop forever reading from the socket and printing.
 *
 * The catch: TCP is a byte stream, not messages. One read() might return
 * half a line, or three lines at once. So:
 *   1. read() into the END of a buffer (buf + used), add to `used`
 *   2. while the buffer contains a '\n' (use memchr):
 *        - replace the '\n' with '\0'
 *        - print_server_line(buf)   <- that's one complete line
 *        - memmove the leftover bytes to the front, shrink `used`
 *   3. repeat
 *
 * If read() returns 0, the server closed the connection: print a notice
 * and exit(0). If it returns < 0, perror and exit(1).
 *
 * Quick version to test first: just read() and fwrite() the raw bytes
 * to stdout, ignore line splitting. Add the buffering once that works.
 */
static void *receive_thread(void *arg)
{
    (void)arg;
    char buf[MAX * 4];
    size_t used = 0;

    for (;;) {
        /* TODO STEP 4: read into buf + used, handle 0 / -1 */

        /* TODO STEP 4: pull out complete lines, call print_server_line() */

        (void)buf;
        (void)used;
        sleep(1);   /* placeholder so the stub doesn't spin; delete this */
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main: G4G connect code + input loop                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    struct sockaddr_in servaddr;
    pthread_t rx_tid;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <username> <conversation_id>\n", argv[0]);
        return 1;
    }
    snprintf(username, sizeof(username), "%s", argv[1]);
    snprintf(conv_id,  sizeof(conv_id),  "%s", argv[2]);

    /* ---- from G4G, unchanged ---- */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(0);
    }
    else
        printf("Socket successfully created..\n");
    bzero(&servaddr, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    servaddr.sin_port = htons(PORT);

    if (connect(sockfd, (SA*)&servaddr, sizeof(servaddr)) != 0) {
        printf("connection with the server failed...\n");
        exit(0);
    }
    else
        printf("connected to the server..\n");
    /* ---- end G4G ---- */

    /* STEP 1 is already done: if you see "connected to the server.."
     * with nc running, you're good. */

    /* STEP 4: start the receive thread. One line:
     *   pthread_create(&rx_tid, NULL, receive_thread, NULL);
     * Check the return value (0 = success). */
    /* TODO STEP 4 */
    (void)rx_tid;

    /* STEP 2: tell the server we're joining this conversation. */
    /* TODO STEP 2: send_request("JOIN", NULL); */

    printf("Joined '%s' as %s. Type messages, /quit to leave.\n",
           conv_id, username);

    /*
     * STEP 3: the input loop.
     *   char input[MAX];
     *   while (fgets(input, sizeof(input), stdin) != NULL) {
     *       - strip the trailing '\n'  (input[strcspn(input, "\n")] = '\0';)
     *       - skip empty lines
     *       - if it's "/quit", break
     *       - otherwise send_request("SEND", input)
     *   }
     * fgets returns NULL on Ctrl-D, which also ends the loop.
     *
     * Note: the server does NOT echo your own messages back to you, so
     * what you typed staying on screen is your only copy. That's fine.
     */
    /* TODO STEP 3 */

    /* STEP 6: clean exit. Send LEAVE, then close. */
    /* TODO STEP 6: send_request("LEAVE", NULL); */

    close(sockfd);   /* from G4G */
    return 0;
}