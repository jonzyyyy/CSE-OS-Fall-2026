/*
 * client.c - CSE 4300 multithreaded chat client
 *
 * Built on the GeeksforGeeks TCP client (socket/connect code in main()).
 * Two threads, so incoming messages are never blocked by the user typing:
 *
 *   main thread    : reads what the user types -> sends JSON to the server
 *   receive thread : reads what the server sends -> prints it
 *
 * Usage:   ./client <username> <conversation_id> [server_ip] [port]
 * Test without the server:   terminal 1: nc -l 8080
 *                            terminal 2: ./client sam 01
 */

#include <arpa/inet.h>   // inet_addr(), htons()
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     // bzero()
#include <sys/socket.h>
#include <unistd.h>      // read(), write(), close()

#define MAX          1024
#define DEFAULT_PORT 8080
#define SA struct sockaddr

/* Shared between both threads. Set once in main() before the receive
 * thread starts and never changed after, so no lock is needed. */
static int  sockfd;
static char username[64];
static char conv_id[64];

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/* write() may send fewer bytes than asked; loop until all are out. */
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

/* Copy src into dst, putting a backslash before " and \ so the result
 * is safe inside a JSON string. Without this, a message containing a
 * quote would break the JSON line. */
static void json_escape(const char *src, char *dst, size_t dstsize)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < dstsize; i++) {
        if (src[i] == '"' || src[i] == '\\')
            dst[j++] = '\\';
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* Build one request line and send it, e.g.
 *   {"conversation_id":"01","type":"SEND","user":"sam","message":"hi"}\n
 * JOIN and LEAVE pass message = NULL, which becomes  "message":null  */
static int send_request(const char *type, const char *message)
{
    char line[MAX * 3];

    if (message == NULL) {
        snprintf(line, sizeof(line),
                 "{\"conversation_id\":\"%s\",\"type\":\"%s\","
                 "\"user\":\"%s\",\"message\":null}\n",
                 conv_id, type, username);
    } else {
        char escaped[MAX * 2];
        json_escape(message, escaped, sizeof(escaped));
        snprintf(line, sizeof(line),
                 "{\"conversation_id\":\"%s\",\"type\":\"%s\","
                 "\"user\":\"%s\",\"message\":\"%s\"}\n",
                 conv_id, type, username, escaped);
    }

    return write_all(sockfd, line, strlen(line));
}

/* ------------------------------------------------------------------ */
/* Receiving                                                           */
/* ------------------------------------------------------------------ */

/* Find "key" in a JSON line and copy its string value into out.
 * Tolerates any spacing around the colon (the server uses two different
 * spacings). Returns 1 if found, 0 if not. Minimal on purpose: no JSON
 * library, just enough for the flat lines the server sends. */
static int get_field(const char *line, const char *key, char *out, size_t outsize)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(line, pattern);
    if (p == NULL)
        return 0;
    p += strlen(pattern);

    while (*p == ' ' || *p == ':')       /* skip  : and spaces */
        p++;
    if (*p != '"')                       /* value isn't a string */
        return 0;
    p++;

    size_t j = 0;
    while (*p != '\0' && *p != '"' && j + 1 < outsize) {
        if (*p == '\\' && p[1] != '\0')  /* undo escaping: \" -> " */
            p++;
        out[j++] = *p++;
    }
    out[j] = '\0';
    return 1;
}

/* Turn one server line into "sender: message". If it doesn't parse,
 * print it raw so nothing is silently lost. */
static void print_server_line(const char *line)
{
    char sender[64], message[MAX];

    if (get_field(line, "sender", sender, sizeof(sender)) &&
        get_field(line, "message", message, sizeof(message)))
        printf("%s: %s\n", sender, message);
    else
        printf("%s\n", line);
    fflush(stdout);
}

/* Runs for the whole session. TCP is a byte stream, so one read() can
 * return half a line or several lines; we buffer bytes and only print
 * once we have a complete line ending in '\n'. */
static void *receive_thread(void *arg)
{
    (void)arg;
    char buf[MAX * 4];
    size_t used = 0;

    for (;;) {
        ssize_t n = read(sockfd, buf + used, sizeof(buf) - used - 1);
        if (n == 0) {
            printf("\n[server closed the connection]\n");
            exit(0);
        }
        if (n < 0) {
            perror("read");
            exit(1);
        }
        used += n;

        /* Print every complete line currently in the buffer. */
        char *nl;
        while ((nl = memchr(buf, '\n', used)) != NULL) {
            *nl = '\0';
            print_server_line(buf);

            size_t line_len = (nl - buf) + 1;   /* including the '\n' */
            memmove(buf, nl + 1, used - line_len);
            used -= line_len;
        }

        /* A single line bigger than the buffer: print what we have
         * rather than getting stuck. */
        if (used == sizeof(buf) - 1) {
            buf[used] = '\0';
            print_server_line(buf);
            used = 0;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    struct sockaddr_in servaddr;
    pthread_t rx_tid;

    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: %s <username> <conversation_id> [server_ip] [port]\n",
                argv[0]);
        return 1;
    }
    snprintf(username, sizeof(username), "%s", argv[1]);
    snprintf(conv_id,  sizeof(conv_id),  "%s", argv[2]);
    const char *server_ip = (argc >= 4) ? argv[3] : "127.0.0.1";
    int port              = (argc == 5) ? atoi(argv[4]) : DEFAULT_PORT;

    /* ---- from G4G ---- */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return 1;
    }
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(server_ip);
    servaddr.sin_port = htons(port);

    if (connect(sockfd, (SA *)&servaddr, sizeof(servaddr)) != 0) {
        perror("connect");
        return 1;
    }
    /* ---- end G4G ---- */

    /* Start listening before JOIN, so the history the server replays
     * in response to JOIN is caught and printed. */
    if (pthread_create(&rx_tid, NULL, receive_thread, NULL) != 0) {
        fprintf(stderr, "failed to start receive thread\n");
        return 1;
    }

    if (send_request("JOIN", NULL) == -1) {
        fprintf(stderr, "failed to join\n");
        return 1;
    }
    printf("Joined '%s' as %s. Type messages, /quit to leave.\n",
           conv_id, username);

    /* Input loop. fgets blocks here while waiting for the user, which is
     * fine: the receive thread keeps printing in the meantime. */
    char input[MAX];
    while (fgets(input, sizeof(input), stdin) != NULL) {   /* NULL = Ctrl-D */
        input[strcspn(input, "\n")] = '\0';                 /* strip newline */
        if (input[0] == '\0')
            continue;
        if (strcmp(input, "/quit") == 0)
            break;
        if (send_request("SEND", input) == -1) {
            fprintf(stderr, "lost connection\n");
            break;
        }
    }

    send_request("LEAVE", NULL);
    close(sockfd);
    return 0;   /* returning from main ends the receive thread too */
}