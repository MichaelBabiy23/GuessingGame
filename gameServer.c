// gameServer.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define BUFFER_SIZE 1024

// ----- Data Structures -----
typedef struct Message {
    char *text;
    struct Message *next;
} Message;

typedef struct Client {
    int fd;
    int id;
    Message *msg_head;
    Message *msg_tail;
    struct Client *next;
} Client;

// ----- Global Variables -----
char *progname = NULL;      // set from argv[0]
int listen_fd;              // listening socket
int max_players;            // maximum concurrent players
int target_number;          // current random target
int game_over = 0;          // flagged when someone wins

Client *client_list = NULL; // linked list of active clients
int len_client_list = 0;    // count of active clients
int max_fd;                 // highest fd in any set

// Heap-allocated fd_sets (only two pointers in .bss)
fd_set *readset = NULL;
fd_set *writeset = NULL;

// ----- Function Prototypes -----
void cleanup_and_exit(int signum);
void usage_and_exit(void);
void parse_arguments(int argc, char *argv[], int *port, int *seed, int *maxPlayers);
void set_socket_nonblocking(int fd);
int create_listening_socket(int port);
Client* add_client(int fd, int assigned_id);
void remove_client(Client **pclient);
int get_available_client_id(void);
void enqueue_message(Client *client, const char *msg);
char* dequeue_message(Client *client);
void broadcast_message(const char *msg, Client *exclude);
void free_message_queue(Client *client);
void free_all_clients(void);
void accept_new_client(void);
void read_from_client(Client **pclient);
void write_to_client(Client **pclient);
void check_if_all_messages_sent(void);
void reset_game(void);

// ----- Signal Handling -----
void cleanup_and_exit(int signum) {
    free_all_clients();
    if (listen_fd > 0) close(listen_fd);
    free(readset);
    free(writeset);
    exit((signum == SIGINT) ? EXIT_SUCCESS : EXIT_FAILURE);
}

// ----- Usage & Argument Parsing -----
void usage_and_exit(void) {
    fprintf(stderr, "Usage: %s <port> <seed> <max-number-of-players>\n", progname);
    cleanup_and_exit(-1);
}

void parse_arguments(int argc, char *argv[], int *port, int *seed, int *maxPlayers) {
    if (argc != 4) usage_and_exit();
    for (int i = 1; i < 4; i++) {
        char *endptr;
        strtol(argv[i], &endptr, 10);
        if (*endptr) usage_and_exit();
    }
    *port = atoi(argv[1]);
    *seed = atoi(argv[2]);
    *maxPlayers = atoi(argv[3]);
    if (*port < 1 || *port > 65535 || *maxPlayers < 2)
        usage_and_exit();
}

// ----- Socket Utility -----
void set_socket_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) { perror("fcntl F_GETFL"); cleanup_and_exit(-1); }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
        cleanup_and_exit(-1);
    }
}

int create_listening_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); cleanup_and_exit(-1); }
    set_socket_nonblocking(sockfd);

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt"); close(sockfd); cleanup_and_exit(-1);
    }

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("bind"); close(sockfd); cleanup_and_exit(-1);
    }
    if (listen(sockfd, max_players) < 0) {
        perror("listen"); close(sockfd); cleanup_and_exit(-1);
    }
    return sockfd;
}

// ----- Client Management -----
Client* add_client(int fd, int assigned_id) {
    Client *c = malloc(sizeof(Client));
    if (!c) { perror("malloc"); cleanup_and_exit(-1); }
    c->fd = fd;
    c->id = assigned_id;
    c->msg_head = c->msg_tail = NULL;
    c->next = client_list;
    client_list = c;
    len_client_list++;
    return c;
}

void remove_client(Client **pclient) {
    if (!pclient || !*pclient) return;
    Client *c = *pclient;

    // unlink from list
    if (client_list == c) {
        client_list = c->next;
    } else {
        Client *prev = client_list;
        while (prev && prev->next != c) prev = prev->next;
        if (prev) prev->next = c->next;
    }
    len_client_list--;

    // notify other players
    char m[BUFFER_SIZE];
    snprintf(m, sizeof(m), "Player %d disconnected\n", c->id);
    broadcast_message(m, c);

    // clear from fd_sets
    if (writeset) FD_CLR(c->fd, writeset);
    if (readset)  FD_CLR(c->fd, readset);

    close(c->fd);
    free_message_queue(c);
    free(c);
    *pclient = NULL;
}

int get_available_client_id(void) {
    int used[max_players+1];
    memset(used, 0, sizeof(used));
    for (Client *c = client_list; c; c = c->next) used[c->id] = 1;
    for (int i = 1; i <= max_players; i++)
        if (!used[i]) return i;
    return -1;
}

// ----- Message Queue Helpers -----
void enqueue_message(Client *client, const char *msg) {
    Message *node = malloc(sizeof(Message));
    if (!node) { perror("malloc"); cleanup_and_exit(-1); }
    node->text = strdup(msg);
    if (!node->text) { perror("strdup"); cleanup_and_exit(-1); }
    node->next = NULL;

    if (!client->msg_head) {
        client->msg_head = client->msg_tail = node;
    } else {
        client->msg_tail->next = node;
        client->msg_tail = node;
    }
    FD_SET(client->fd, writeset);
}

char* dequeue_message(Client *client) {
    if (!client->msg_head) return NULL;
    Message *m = client->msg_head;
    char *txt = strdup(m->text);
    client->msg_head = m->next;
    if (!client->msg_head) client->msg_tail = NULL;
    free(m->text);
    free(m);
    return txt;
}

void broadcast_message(const char *msg, Client *exclude) {
    for (Client *c = client_list; c; c = c->next)
        if (c != exclude)
            enqueue_message(c, msg);
}

void free_message_queue(Client *client) {
    Message *cur = client->msg_head;
    while (cur) {
        Message *next = cur->next;
        free(cur->text);
        free(cur);
        cur = next;
    }
    client->msg_head = client->msg_tail = NULL;
}

// ----- Free All Clients -----
void free_all_clients(void) {
    Client *cur = client_list;
    while (cur) {
        Client *next = cur->next;
        close(cur->fd);
        free_message_queue(cur);
        free(cur);
        cur = next;
    }
    client_list = NULL;
    len_client_list = 0;

    // only reset fd_sets if allocated
    if (writeset) FD_ZERO(writeset);
    if (readset) {
        FD_ZERO(readset);
        FD_SET(listen_fd, readset);
    }
}

// ----- Game Logic -----
void reset_game(void) {
    target_number = (rand() % 100) + 1;
    game_over = 0;
}

// ----- I/O Handling -----
void accept_new_client(void) {
    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);
    int fd = accept(listen_fd, (struct sockaddr *)&cli, &len);
    if (fd < 0) return;
    set_socket_nonblocking(fd);

    int id = get_available_client_id();
    if (id < 0) { close(fd); return; }

    Client *c = add_client(fd, id);
    char buf[BUFFER_SIZE];
    snprintf(buf, sizeof(buf), "Welcome to the game, your id is %d\n", id);
    enqueue_message(c, buf);

    FD_SET(fd, readset);
    FD_SET(fd, writeset);
    if (fd > max_fd) max_fd = fd;

    snprintf(buf, sizeof(buf), "Player %d joined the game\n", id);
    broadcast_message(buf, c);
}

void read_from_client(Client **pclient) {
    Client *c = *pclient;
    char buf[BUFFER_SIZE];
    int n = read(c->fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        remove_client(pclient);
        return;
    }
    buf[n] = '\0';
    int guess = atoi(buf);
    char msg[BUFFER_SIZE];

    snprintf(msg, sizeof(msg), "Player %d guessed %d\n", c->id, guess);
    broadcast_message(msg, NULL);

    if (guess < target_number) {
        snprintf(msg, sizeof(msg), "The guess %d is too low\n", guess);
        broadcast_message(msg, NULL);
    } else if (guess > target_number) {
        snprintf(msg, sizeof(msg), "The guess %d is too high\n", guess);
        broadcast_message(msg, NULL);
    } else {
        snprintf(msg, sizeof(msg), "Player %d wins\n", c->id);
        broadcast_message(msg, NULL);
        snprintf(msg, sizeof(msg), "The correct guessing is %d\n", guess);
        broadcast_message(msg, NULL);
        game_over = 1;
    }
}

void write_to_client(Client **pclient) {
    Client *c = *pclient;
    char *msg = dequeue_message(c);
    if (msg) {
        write(c->fd, msg, strlen(msg));
        free(msg);
    }
    if (!c->msg_head)
        FD_CLR(c->fd, writeset);
    // no per-client removal here
}

void check_if_all_messages_sent(void) {
    if (!game_over) return;
    for (Client *c = client_list; c; c = c->next)
        if (c->msg_head) return;

    // tear down all clients at once
    for (Client *c = client_list; c; c = c->next) {
        close(c->fd);
        if (readset)  FD_CLR(c->fd, readset);
        if (writeset) FD_CLR(c->fd, writeset);
    }
    free_all_clients();
    reset_game();
}

// ----- Main -----
int main(int argc, char *argv[]) {
    progname = argv[0];

    int port_arg, seed;
    parse_arguments(argc, argv, &port_arg, &seed, &max_players);

    signal(SIGINT, cleanup_and_exit);
    srand(seed);
    reset_game();

    listen_fd = create_listening_socket(port_arg);

    // allocate our fd_sets on the heap
    readset  = malloc(sizeof(fd_set));
    writeset = malloc(sizeof(fd_set));
    if (!readset || !writeset) {
        perror("malloc");
        cleanup_and_exit(-1);
    }

    FD_ZERO(readset);
    FD_ZERO(writeset);
    FD_SET(listen_fd, readset);

    max_fd = listen_fd;

    while (1) {
        fd_set tmp_read  = *readset;
        fd_set tmp_write = *writeset;

        // recalc max_fd
        max_fd = listen_fd;
        for (Client *c = client_list; c; c = c->next)
            if (c->fd > max_fd) max_fd = c->fd;

        int ready = select(max_fd + 1, &tmp_read, &tmp_write, NULL, NULL);
        if (ready < 0 && errno != EINTR) {
            perror("select");
            cleanup_and_exit(-1);
        }

        if (FD_ISSET(listen_fd, &tmp_read)) {
            printf("Server is ready to read from welcome socket %d\n", listen_fd);
            accept_new_client();
        }

        Client *cur = client_list;
        while (cur) {
            Client *next = cur->next;

            if (cur->fd > 0 && FD_ISSET(cur->fd, &tmp_write)) {
                printf("Server is ready to write to player %d on socket %d\n",
                       cur->id, cur->fd);
                write_to_client(&cur);
            }
            if (cur->fd > 0 && FD_ISSET(cur->fd, &tmp_read)) {
                printf("Server is ready to read from player %d on socket %d\n",
                       cur->id, cur->fd);
                read_from_client(&cur);
            }

            cur = next;
        }

        check_if_all_messages_sent();

        if (len_client_list >= max_players)
            FD_CLR(listen_fd, readset);
        else
            FD_SET(listen_fd, readset);
    }

    return 0;
}
