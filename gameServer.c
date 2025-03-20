/*
 * gameServer.c - "Guess the Number" game server
 *
 * - Validates usage arguments: <port> <seed> <max-number-of-players>
 * - Non-blocking sockets, single select() call per loop
 * - Prints readiness messages whenever a socket is ready for read/write
 * - Sends one line per write event (partial write handling with offset)
 * - Closes connections only after all messages (including winning lines) are sent
 * - Frees all resources properly
 *
 * Usage: ./game_server <port> <seed> <max-number-of-players>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define INITIAL_BUFFER_SIZE 1024
#define TARGET_RANGE 100

/* ----- Data Structures ----- */

typedef struct message {
    char *text;           /* The complete message, ending with \n */
    int length;           /* Total length in bytes */
    int sent_offset;      /* How many bytes have been sent so far */
    struct message *next;
} message_t;

typedef struct client {
    int fd;
    int id;
    message_t *msg_head;
    message_t *msg_tail;
    char *in_buffer;
    int in_buffer_size;
    int in_buffer_capacity;
    struct client *next;
} client_t;

/* ----- Globals ----- */

int server_fd = -1;
client_t *clients = NULL;
int max_players = 0;
int current_target = 0;
int port = 0;
sig_atomic_t exit_requested = 0;

/*
 * game_over indicates that someone has guessed correctly. We don't close
 * connections immediately; we wait until all queued messages are fully sent.
 */
int game_over = 0;

/* ----- Prototypes ----- */

void sigint_handler(int sig);
void init_signal_handler(void);
void usage_and_exit(void);
void parse_arguments(int argc, char *argv[]);
void set_socket_nonblocking(int fd);
int create_listening_socket(int port);
client_t *create_client(int fd, int id);
void add_client(client_t *client);
void remove_client(client_t *client);
int get_available_client_id(void);
void enqueue_message(client_t *client, const char *text);
void broadcast_message(const char *text, client_t *skip);
void append_to_client_buffer(client_t *client, const char *data, int len);
void process_client_messages(client_t *client);
void accept_new_connection(void);
void read_from_client(client_t *client);
void write_to_client(client_t *client);
void check_if_all_messages_sent(void);
void start_new_game(void);
void cleanup_all(void);

/* ----- Signal Handling ----- */

void sigint_handler(int sig) {
    (void)sig;
    exit_requested = 1;
}

void init_signal_handler(void) {
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

/* ----- Usage and Argument Parsing ----- */

void usage_and_exit(void) {
    fprintf(stderr, "Usage: ./game_server <port> <seed> <max-number-of-players>\n");
    exit(EXIT_FAILURE);
}

void parse_arguments(int argc, char *argv[]) {
    if (argc != 4) {
        usage_and_exit();
    }
    char *endptr = NULL;

    long port_val = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || port_val < 1 || port_val > 65535) {
        usage_and_exit();
    }
    port = (int)port_val;

    endptr = NULL;
    long seed_val = strtol(argv[2], &endptr, 10);
    if (*endptr != '\0') {
        usage_and_exit();
    }
    srand((unsigned)seed_val);

    endptr = NULL;
    long mp_val = strtol(argv[3], &endptr, 10);
    if (*endptr != '\0' || mp_val < 2) {
        usage_and_exit();
    }
    max_players = (int)mp_val;
}

/* ----- Socket Utility ----- */

void set_socket_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        exit(EXIT_FAILURE);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        exit(EXIT_FAILURE);
    }
}

int create_listening_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    set_socket_nonblocking(sockfd);

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if (listen(sockfd, max_players) == -1) {
        perror("listen");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    return sockfd;
}

/* ----- Client Management ----- */

client_t *create_client(int fd, int id) {
    client_t *c = malloc(sizeof(client_t));
    if (!c) {
        perror("malloc");
        return NULL;
    }
    c->fd = fd;
    c->id = id;
    c->msg_head = c->msg_tail = NULL;
    c->in_buffer_capacity = INITIAL_BUFFER_SIZE;
    c->in_buffer = malloc(c->in_buffer_capacity);
    if (!c->in_buffer) {
        perror("malloc");
        free(c);
        return NULL;
    }
    c->in_buffer_size = 0;
    c->next = NULL;
    return c;
}

void add_client(client_t *client) {
    client->next = clients;
    clients = client;
}

void remove_client(client_t *client) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Player %d disconnected\n", client->id);
    broadcast_message(msg, client);

    if (clients == client) {
        clients = client->next;
    } else {
        client_t *prev = clients;
        while (prev && prev->next != client)
            prev = prev->next;
        if (prev)
            prev->next = client->next;
    }
    close(client->fd);

    free(client->in_buffer);
    message_t *m = client->msg_head;
    while (m) {
        message_t *tmp = m;
        m = m->next;
        free(tmp->text);
        free(tmp);
    }
    free(client);
}

int get_available_client_id(void) {
    int used[max_players + 1];
    memset(used, 0, sizeof(used));
    client_t *cur = clients;
    while (cur) {
        if (cur->id >= 1 && cur->id <= max_players)
            used[cur->id] = 1;
        cur = cur->next;
    }
    for (int i = 1; i <= max_players; i++)
        if (!used[i])
            return i;
    return -1;
}

/* ----- Message Queue Helpers ----- */

void enqueue_message(client_t *client, const char *text) {
    message_t *msg = malloc(sizeof(message_t));
    if (!msg) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    msg->text = strdup(text);
    if (!msg->text) {
        perror("strdup");
        free(msg);
        exit(EXIT_FAILURE);
    }
    msg->length = (int)strlen(msg->text);
    msg->sent_offset = 0;
    msg->next = NULL;

    if (client->msg_tail)
        client->msg_tail->next = msg;
    else
        client->msg_head = msg;
    client->msg_tail = msg;
}

void broadcast_message(const char *text, client_t *skip) {
    client_t *cur = clients;
    while (cur) {
        if (cur != skip)
            enqueue_message(cur, text);
        cur = cur->next;
    }
}

/* ----- Buffer and Message Processing ----- */

void append_to_client_buffer(client_t *client, const char *data, int len) {
    if (client->in_buffer_size + len > client->in_buffer_capacity) {
        int new_cap = client->in_buffer_capacity * 2;
        while (client->in_buffer_size + len > new_cap)
            new_cap *= 2;
        char *new_buf = realloc(client->in_buffer, new_cap);
        if (!new_buf) {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        client->in_buffer = new_buf;
        client->in_buffer_capacity = new_cap;
    }
    memcpy(client->in_buffer + client->in_buffer_size, data, len);
    client->in_buffer_size += len;
}

void process_client_messages(client_t *client) {
    int start = 0;
    for (int i = 0; i < client->in_buffer_size; i++) {
        if (client->in_buffer[i] == '\n') {
            int line_len = i - start;
            char *line = malloc(line_len + 1);
            if (!line) {
                perror("malloc");
                exit(EXIT_FAILURE);
            }
            memcpy(line, client->in_buffer + start, line_len);
            line[line_len] = '\0';

            if (line_len > 0 && line[line_len - 1] == '\r')
                line[line_len - 1] = '\0';

            if (strcmp(line, "quit") == 0) {
                free(line);
                remove_client(client);
                return;
            } else {
                int guess = atoi(line);
                char msg[128];
                snprintf(msg, sizeof(msg), "Player %d guessed %d\n", client->id, guess);
                broadcast_message(msg, NULL);

                if (!game_over) {
                    if (guess < current_target) {
                        snprintf(msg, sizeof(msg), "The guess %d is too low\n", guess);
                        broadcast_message(msg, NULL);
                    } else if (guess > current_target) {
                        snprintf(msg, sizeof(msg), "The guess %d is too high\n", guess);
                        broadcast_message(msg, NULL);
                    } else {
                        snprintf(msg, sizeof(msg), "Player %d wins\n", client->id);
                        broadcast_message(msg, NULL);
                        snprintf(msg, sizeof(msg), "The correct guessing is %d\n", guess);
                        broadcast_message(msg, NULL);
                        game_over = 1;
                    }
                }
            }
            free(line);
            start = i + 1;
        }
    }
    if (start > 0) {
        if (start < client->in_buffer_size)
            memmove(client->in_buffer, client->in_buffer + start, client->in_buffer_size - start);
        client->in_buffer_size -= start;
    }
}

/* ----- I/O Handling ----- */

void accept_new_connection(void) {
    struct sockaddr_in caddr;
    socklen_t addrlen = sizeof(caddr);
    int new_fd = accept(server_fd, (struct sockaddr *)&caddr, &addrlen);
    if (new_fd >= 0) {
        printf("Server is ready to read from welcome socket %d\n", server_fd);
        set_socket_nonblocking(new_fd);
        int new_id = get_available_client_id();
        if (new_id == -1) {
            close(new_fd);
        } else {
            client_t *c = create_client(new_fd, new_id);
            if (!c) {
                close(new_fd);
            } else {
                add_client(c);
                char buf[128];
                snprintf(buf, sizeof(buf), "Welcome to the game, your id is %d\n", new_id);
                enqueue_message(c, buf);

                snprintf(buf, sizeof(buf), "Player %d joined the game\n", new_id);
                broadcast_message(buf, c);
            }
        }
    }
}

void read_from_client(client_t *client) {
    printf("Server is ready to read from player %d on socket %d\n", client->id, client->fd);
    char buf[512];
    int bytes = recv(client->fd, buf, sizeof(buf), 0);
    if (bytes <= 0) {
        remove_client(client);
    } else {
        append_to_client_buffer(client, buf, bytes);
        process_client_messages(client);
    }
}

void write_to_client(client_t *client) {
    if (!client->msg_head) return;

    printf("Server is ready to write to player %d on socket %d\n", client->id, client->fd);
    message_t *msg = client->msg_head;
    int remaining = msg->length - msg->sent_offset;
    int sent = send(client->fd, msg->text + msg->sent_offset, remaining, 0);
    if (sent > 0) {
        msg->sent_offset += sent;
        if (msg->sent_offset == msg->length) {
            client->msg_head = msg->next;
            if (!client->msg_head)
                client->msg_tail = NULL;
            free(msg->text);
            free(msg);
        }
    } else if (sent < 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            remove_client(client);
        }
    }
}

/*
 * If the game ended (someone guessed correctly), we check if
 * all outgoing queues are empty. If so, we close all clients
 * and start a new game.
 */
void check_if_all_messages_sent(void) {
    if (!game_over) return;
    client_t *c = clients;
    while (c) {
        if (c->msg_head) {
            return;
        }
        c = c->next;
    }
    c = clients;
    while (c) {
        close(c->fd);
        c = c->next;
    }
    while (clients) {
        client_t *tmp = clients;
        clients = clients->next;
        free(tmp->in_buffer);
        message_t *m = tmp->msg_head;
        while (m) {
            message_t *t2 = m;
            m = m->next;
            free(t2->text);
            free(t2);
        }
        free(tmp);
    }
    start_new_game();
}

void start_new_game(void) {
    current_target = (rand() % TARGET_RANGE) + 1;
    game_over = 0;
    printf("New game started. New target generated.\n");
}

/* ----- Cleanup ----- */

void cleanup_all(void) {
    client_t *c = clients;
    while (c) {
        close(c->fd);
        free(c->in_buffer);
        message_t *m = c->msg_head;
        while (m) {
            message_t *tmp = m;
            m = m->next;
            free(tmp->text);
            free(tmp);
        }
        client_t *tmpc = c;
        c = c->next;
        free(tmpc);
    }
    if (server_fd != -1) {
        close(server_fd);
    }
}

/* ----- Main ----- */

int main(int argc, char *argv[]) {
    parse_arguments(argc, argv);
    init_signal_handler();

    current_target = (rand() % TARGET_RANGE) + 1;
    server_fd = create_listening_socket(port);
    printf("Server listening on port %d. Target number is generated.\n", port);

    while (1) {
        sigset_t newmask, oldmask;
        sigemptyset(&newmask);
        sigaddset(&newmask, SIGINT);
        sigprocmask(SIG_BLOCK, &newmask, &oldmask);
        int local_exit = exit_requested;
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        if (local_exit)
            break;

        check_if_all_messages_sent();

        fd_set read_set, write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);

        int max_fd = server_fd;
        FD_SET(server_fd, &read_set);

        client_t *c = clients;
        while (c) {
            FD_SET(c->fd, &read_set);
            if (c->msg_head) {
                FD_SET(c->fd, &write_set);
            }
            if (c->fd > max_fd) {
                max_fd = c->fd;
            }
            c = c->next;
        }

        int activity = select(max_fd + 1, &read_set, &write_set, NULL, NULL);
        if (activity < 0 && errno != EINTR) {
            perror("select");
            cleanup_all();
            exit(EXIT_FAILURE);
        }

        if (FD_ISSET(server_fd, &read_set)) {
            /* The welcome socket is ready to read (accept a new connection). */
            /* We'll print "Server is ready to read from welcome socket" inside accept_new_connection(). */
            accept_new_connection();
        }

        c = clients;
        while (c) {
            client_t *next = c->next;
            if (FD_ISSET(c->fd, &read_set)) {
                read_from_client(c);
            }
            c = next;
        }

        c = clients;
        while (c) {
            client_t *next = c->next;
            if (FD_ISSET(c->fd, &write_set) && c->msg_head) {
                write_to_client(c);
            }
            c = next;
        }
    }

    cleanup_all();
    return 0;
}
