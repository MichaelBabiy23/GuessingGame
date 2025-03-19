/* gameServer.c - "Guess the Number" game server
 *
 * Usage: ./server <port> <seed> <max-number-of-players>
 * Compile with: gcc -Wall -Wextra -o server gameServer.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define INITIAL_BUFFER_SIZE 1024
#define TARGET_RANGE 100

/* Function prototypes */
void sigint_handler(int sig);
void init_signal_handler(void);
void set_socket_nonblocking(int fd);
int create_listening_socket(int port);
struct client *create_client(int fd, int id);
void add_client(struct client *client);
void remove_client(struct client *client);
int get_available_client_id(void);
void enqueue_message(struct client *client, const char *text);
struct message *dequeue_message(struct client *client);
void broadcast_message(const char *text, struct client *skip_client);
void append_to_client_buffer(struct client *client, const char *data, int len);
void process_client_messages(struct client *client);
void flush_client_queues(void);
void accept_new_connection(void);
void read_from_client(struct client *client);
void write_to_client(struct client *client);
void cleanup_all(void);

/* Data structures */
typedef struct message {
    char *text;
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

/* Global variables */
int server_fd = -1;
client_t *clients = NULL;
int max_players = 0;
int current_target = 0;
int port = 0;
sig_atomic_t exit_requested = 0;

/* Signal handling */
void sigint_handler(int sig) {
    (void)sig;
    exit_requested = 1;
}

void init_signal_handler(void) {
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sa.sa_flags   = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

/* Socket utility functions */
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

/* Client management */
client_t *create_client(int fd, int id) {
    client_t *client = malloc(sizeof(client_t));
    if (!client) {
        perror("malloc");
        return NULL;
    }
    client->fd = fd;
    client->id = id;
    client->msg_head = client->msg_tail = NULL;
    client->in_buffer_capacity = INITIAL_BUFFER_SIZE;
    client->in_buffer = malloc(client->in_buffer_capacity);
    if (!client->in_buffer) {
        perror("malloc");
        free(client);
        return NULL;
    }
    client->in_buffer_size = 0;
    client->next = NULL;
    return client;
}

void add_client(client_t *client) {
    client->next = clients;
    clients = client;
}

void remove_client(client_t *client) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Player %d disconnected\n", client->id);
    broadcast_message(msg, client);

    if (clients == client)
        clients = client->next;
    else {
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
    int used[max_players+1];
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

/* Message queue helpers */
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
    msg->next = NULL;
    if (client->msg_tail)
        client->msg_tail->next = msg;
    else
        client->msg_head = msg;
    client->msg_tail = msg;
}

message_t *dequeue_message(client_t *client) {
    message_t *msg = client->msg_head;
    if (msg) {
        client->msg_head = msg->next;
        if (!client->msg_head)
            client->msg_tail = NULL;
    }
    return msg;
}

void broadcast_message(const char *text, client_t *skip_client) {
    client_t *cur = clients;
    while (cur) {
        if (cur != skip_client)
            enqueue_message(cur, text);
        cur = cur->next;
    }
}

/* Buffer and message processing */
void append_to_client_buffer(client_t *client, const char *data, int len) {
    if (client->in_buffer_size + len > client->in_buffer_capacity) {
        int new_capacity = client->in_buffer_capacity * 2;
        while (client->in_buffer_size + len > new_capacity)
            new_capacity *= 2;
        char *new_buffer = realloc(client->in_buffer, new_capacity);
        if (!new_buffer) {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        client->in_buffer = new_buffer;
        client->in_buffer_capacity = new_capacity;
    }
    memcpy(client->in_buffer + client->in_buffer_size, data, len);
    client->in_buffer_size += len;
}

void process_client_messages(client_t *client) {
    int processed = 0;
    for (int i = 0; i < client->in_buffer_size; i++) {
        if (client->in_buffer[i] == '\n') {
            int line_length = i - processed;
            char *line = malloc(line_length + 1);
            if (!line) {
                perror("malloc");
                exit(EXIT_FAILURE);
            }
            memcpy(line, client->in_buffer + processed, line_length);
            line[line_length] = '\0';
            if (line_length > 0 && line[line_length - 1] == '\r')
                line[line_length - 1] = '\0';
            if (strcmp(line, "quit") == 0) {
                free(line);
                remove_client(client);
                return;
            } else {
                int guess = atoi(line);
                char guess_msg[256];
                snprintf(guess_msg, sizeof(guess_msg), "Player %d guessed %d\n", client->id, guess);
                broadcast_message(guess_msg, NULL);
                char response[256];
                if (guess < current_target) {
                    snprintf(response, sizeof(response), "The guess %d is too low\n", guess);
                    broadcast_message(response, NULL);
                } else if (guess > current_target) {
                    snprintf(response, sizeof(response), "The guess %d is too high\n", guess);
                    broadcast_message(response, NULL);
                } else {
                    snprintf(response, sizeof(response), "Player %d wins\nThe correct guessing is %d\n", client->id, guess);
                    broadcast_message(response, NULL);
                    client_t *cur = clients;
                    while (cur) {
                        while (cur->msg_head) {
                            message_t *msg = dequeue_message(cur);
                            if (msg) {
                                send(cur->fd, msg->text, strlen(msg->text), 0);
                                free(msg->text);
                                free(msg);
                            }
                        }
                        cur = cur->next;
                    }
                    sleep(1);
                    cur = clients;
                    while (cur) {
                        close(cur->fd);
                        cur = cur->next;
                    }
                    while (clients) {
                        client_t *tmp = clients;
                        clients = clients->next;
                        free(tmp->in_buffer);
                        message_t *m = tmp->msg_head;
                        while (m) {
                            message_t *tmp_m = m;
                            m = m->next;
                            free(tmp_m->text);
                            free(tmp_m);
                        }
                        free(tmp);
                    }
                    current_target = (rand() % TARGET_RANGE) + 1;
                    printf("New game started. New target generated.\n");
                    free(line);
                    return;
                }
            }
            free(line);
            processed = i + 1;
        }
    }
    if (processed > 0) {
        if (processed < client->in_buffer_size)
            memmove(client->in_buffer, client->in_buffer + processed, client->in_buffer_size - processed);
        client->in_buffer_size -= processed;
    }
}

void flush_client_queues(void) {
    client_t *cur = clients;
    while (cur) {
        while (cur->msg_head) {
            message_t *msg = dequeue_message(cur);
            if (msg) {
                send(cur->fd, msg->text, strlen(msg->text), 0);
                free(msg->text);
                free(msg);
            }
        }
        cur = cur->next;
    }
}

/* I/O handling */
void accept_new_connection(void) {
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    int new_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
    if (new_fd != -1) {
        set_socket_nonblocking(new_fd);
        int new_id = get_available_client_id();
        if (new_id == -1) {
            close(new_fd);
        } else {
            client_t *new_client = create_client(new_fd, new_id);
            if (!new_client) {
                close(new_fd);
            } else {
                add_client(new_client);
                char welcome_msg[256];
                snprintf(welcome_msg, sizeof(welcome_msg), "Welcome to the game, your id is %d\n", new_id);
                enqueue_message(new_client, welcome_msg);
                char join_msg[256];
                snprintf(join_msg, sizeof(join_msg), "Player %d joined the game\n", new_id);
                broadcast_message(join_msg, new_client);
            }
        }
    }
}

void read_from_client(client_t *client) {
    char buffer[512];
    int bytes = recv(client->fd, buffer, sizeof(buffer), 0);
    if (bytes <= 0) {
        remove_client(client);
    } else {
        append_to_client_buffer(client, buffer, bytes);
        process_client_messages(client);
    }
}

void write_to_client(client_t *client) {
    if (client->msg_head) {
        message_t *msg = dequeue_message(client);
        if (msg) {
            int sent = send(client->fd, msg->text, strlen(msg->text), 0);
            if (sent == -1) {
                remove_client(client);
            }
            free(msg->text);
            free(msg);
        }
    }
}

/* Cleanup */
void cleanup_all(void) {
    client_t *cur = clients;
    while (cur) {
        close(cur->fd);
        free(cur->in_buffer);
        message_t *m = cur->msg_head;
        while (m) {
            message_t *tmp = m;
            m = m->next;
            free(tmp->text);
            free(tmp);
        }
        client_t *tmp_client = cur;
        cur = cur->next;
        free(tmp_client);
    }
    if (server_fd != -1)
        close(server_fd);
}

/* Main */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: ./server <port> <seed> <max-number-of-players>\n");
        exit(EXIT_FAILURE);
    }

    port = atoi(argv[1]);
    int seed = atoi(argv[2]);
    max_players = atoi(argv[3]);
    if (port < 1 || port > 65535 || max_players <= 1) {
        fprintf(stderr, "Usage: ./server <port> <seed> <max-number-of-players>\n");
        exit(EXIT_FAILURE);
    }

    srand(seed);
    current_target = (rand() % TARGET_RANGE) + 1;
    init_signal_handler();
    server_fd = create_listening_socket(port);
    printf("Server listening on port %d. A target number is generated.\n", port);

    while (1) {
        sigset_t newmask, oldmask;
        sigemptyset(&newmask);
        sigaddset(&newmask, SIGINT);
        sigprocmask(SIG_BLOCK, &newmask, &oldmask);
        int local_exit = exit_requested;
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        if (local_exit)
            break;

        fd_set read_set, write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        int max_fd = server_fd;
        FD_SET(server_fd, &read_set);
        client_t *cur = clients;
        while (cur) {
            FD_SET(cur->fd, &read_set);
            if (cur->msg_head)
                FD_SET(cur->fd, &write_set);
            if (cur->fd > max_fd)
                max_fd = cur->fd;
            cur = cur->next;
        }

        int activity = select(max_fd + 1, &read_set, &write_set, NULL, NULL);
        if (activity < 0 && errno != EINTR) {
            perror("select");
            cleanup_all();
            exit(EXIT_FAILURE);
        }

        if (FD_ISSET(server_fd, &read_set))
            accept_new_connection();

        cur = clients;
        while (cur) {
            client_t *next = cur->next;
            if (FD_ISSET(cur->fd, &read_set))
                read_from_client(cur);
            cur = next;
        }

        cur = clients;
        while (cur) {
            client_t *next = cur->next;
            if (FD_ISSET(cur->fd, &write_set) && cur->msg_head)
                write_to_client(cur);
            cur = next;
        }
    }

    cleanup_all();
    return 0;
}
