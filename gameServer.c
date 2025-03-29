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

// Message node for a client's outgoing message queue.
typedef struct Message {
    char *text;             // Dynamically allocated message (ends with '\n')
    struct Message *next;
} Message;

// Client structure.
typedef struct Client {
    int fd;                 // Client's socket descriptor
    int id;                 // Assigned id (1 to max-number-of-players)
    Message *msg_head;      // Head of outgoing message queue
    Message *msg_tail;      // Tail of outgoing message queue
    struct Client *next;    // Pointer to next client in linked list
} Client;

// ----- Global Variables -----
int listen_fd;                  // Listening socket
int max_players;                // Maximum allowed players
int target_number;              // Current target number to guess
Client *client_list = NULL;     // Linked list of active clients
sig_atomic_t exit_requested = 0;
int game_over = 0;              // Set to 1 when a correct guess occurs
fd_set readset, writeset;

// For select() management.
int max_fd;                     // Highest file descriptor currently in use
int len_client_list = 0;        // Current number of connected clients

// ----- Function Prototypes -----
void cleanup_and_exit(int signum);
void usage_and_exit(void);
void parse_arguments(int argc, char *argv[], int *port, int *seed, int *maxPlayers);
void set_socket_nonblocking(int fd);
int create_listening_socket(int port);
Client* add_client(int fd, int assigned_id);
void remove_client(Client **client);
int get_available_client_id(void);
void enqueue_message(Client *client, const char *msg);
char* dequeue_message(Client *client);
void broadcast_message(const char *msg, Client *exclude);
void free_message_queue(Client *client);
void free_all_clients(void);
void accept_new_client();
void read_from_client(Client **client);
void write_to_client(Client *client);
void check_if_all_messages_sent(fd_set *readset, fd_set *writeset);
void start_new_game(void);

// ----- Signal Handling -----
void cleanup_and_exit(int signum) {
    (void)signum;
    free_all_clients();
    if (listen_fd > 0)
        close(listen_fd);
    exit(EXIT_SUCCESS);
}

// ----- Usage & Argument Parsing -----
void usage_and_exit(void) {
    fprintf(stderr, "Usage: ./server <port> <seed> <max-number-of-players>\n");
    exit(EXIT_FAILURE);
}

void parse_arguments(int argc, char *argv[], int *port, int *seed, int *maxPlayers) {
    if (argc != 4)
        usage_and_exit();
    for (int i = 1; i < 4; i++) {
        char *endptr;
        strtol(argv[i], &endptr, 10);
        if (*endptr != '\0')
            usage_and_exit();
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
    if (flags < 0) {
        perror("fcntl F_GETFL");
        exit(EXIT_FAILURE);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
        exit(EXIT_FAILURE);
    }
}

int create_listening_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    set_socket_nonblocking(sockfd);

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    if (listen(sockfd, max_players) < 0) {
        perror("listen");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    return sockfd;
}

// ----- Client Management -----
Client* add_client(int fd, int assigned_id) {
    Client *new_client = malloc(sizeof(Client));
    if (!new_client) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    new_client->fd = fd;
    new_client->id = assigned_id;
    new_client->msg_head = new_client->msg_tail = NULL;
    new_client->next = client_list;
    client_list = new_client;
    len_client_list++;
    return new_client;
}

void remove_client(Client **client) {
    Client *to_remove = *client;
    // Remove to_remove from the global linked list.
    if (client_list == to_remove) {
        client_list = to_remove->next;
    } else {
        Client *cur = client_list;
        while (cur && cur->next != to_remove)
            cur = cur->next;
        if (cur)
            cur->next = to_remove->next;
    }
    // Free the message queue for this client.
    free_message_queue(to_remove);
    // Remove file descriptor from global FD sets.
    FD_CLR(to_remove->fd, &writeset);
    FD_CLR(to_remove->fd, &readset);
    len_client_list--;  // Decrement the count.
    close(to_remove->fd);
    to_remove->fd = 0;
    // Free the client structure.
    free(to_remove);
    // Set the pointer to NULL to avoid dangling pointer issues.
    *client = NULL;
}

int get_available_client_id(void) {
    int used[max_players + 1];
    memset(used, 0, sizeof(used));
    for (Client *cur = client_list; cur != NULL; cur = cur->next)
        used[cur->id] = 1;
    for (int i = 1; i <= max_players; i++) {
        if (!used[i])
            return i;
    }
    return -1;
}

// ----- Message Queue Helpers -----
void enqueue_message(Client *client, const char *msg) {
    Message *node = malloc(sizeof(Message));
    if (!node) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    node->text = strdup(msg);
    if (!node->text) {
        perror("strdup");
        free(node);
        exit(EXIT_FAILURE);
    }
    node->next = NULL;
    if (!client->msg_head)
        client->msg_head = client->msg_tail = node;
    else {
        client->msg_tail->next = node;
        client->msg_tail = node;
    }
    FD_SET(client->fd, &writeset);
}

char* dequeue_message(Client *client) {
    if (!client->msg_head)
        return NULL;
    Message *node = client->msg_head;
    char *msg = strdup(node->text);
    client->msg_head = node->next;
    if (!client->msg_head)
        client->msg_tail = NULL;
    free(node->text);
    free(node);
    return msg;
}

void broadcast_message(const char *msg, Client *exclude) {
    for (Client *cur = client_list; cur != NULL; cur = cur->next) {
        if (cur != exclude)
            enqueue_message(cur, msg);
    }
}

void free_message_queue(Client *client) {
    Message *cur = client->msg_head;
    while (cur) {
        Message *tmp = cur;
        cur = cur->next;
        free(tmp->text);
        free(tmp);
    }
    client->msg_head = client->msg_tail = NULL;
}

void free_all_clients(void) {
    while (client_list)
        remove_client(&client_list);
}

// ----- Game Logic -----
void reset_game(void) {
    target_number = (rand() % 100) + 1;
    game_over = 0;
    printf("New game started. New target generated.\n");
}

// ----- I/O Handling -----
void accept_new_client() {
    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);
    int fd = accept(listen_fd, (struct sockaddr *)&cli, &len);
    if (fd < 0)
        return;
    set_socket_nonblocking(fd);
    int id = get_available_client_id();
    if (id == -1) {
        close(fd);
        return;
    }
    Client *new_client = add_client(fd, id);
    char buf[BUFFER_SIZE];
    snprintf(buf, BUFFER_SIZE, "Welcome to the game, your id is %d\n", id);
    enqueue_message(new_client, buf);
    FD_SET(fd, &readset);
    FD_SET(fd, &writeset);
    if (fd > max_fd)
        max_fd = fd;
    snprintf(buf, BUFFER_SIZE, "Player %d joined the game\n", id);
    broadcast_message(buf, new_client);
}

void read_from_client(Client **client) {
    char buf[BUFFER_SIZE];
    int n = read((*client)->fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        char msg[BUFFER_SIZE];
        snprintf(msg, BUFFER_SIZE, "Player %d disconnected\n", (*client)->id);
        broadcast_message(msg, *client);
        remove_client(client);
        return;
    } else {
        buf[n] = '\0';
        int guess = atoi(buf);
        char msg[BUFFER_SIZE];
        snprintf(msg, BUFFER_SIZE, "Player %d guessed %d\n", (*client)->id, guess);
        broadcast_message(msg, NULL);
        if (guess < target_number) {
            snprintf(msg, BUFFER_SIZE, "The guess %d is too low\n", guess);
            broadcast_message(msg, NULL);
        } else if (guess > target_number) {
            snprintf(msg, BUFFER_SIZE, "The guess %d is too high\n", guess);
            broadcast_message(msg, NULL);
        } else {
            snprintf(msg, BUFFER_SIZE, "Player %d wins\n", (*client)->id);
            broadcast_message(msg, NULL);
            snprintf(msg, BUFFER_SIZE, "The correct guessing is %d\n", guess);
            broadcast_message(msg, NULL);
            game_over = 1;
        }
    }
}

void write_to_client(Client *client) {
    char *msg = dequeue_message(client);
    if (msg) {
        write(client->fd, msg, strlen(msg));
        free(msg);
    }
    if (!client->msg_head)
        FD_CLR(client->fd, &writeset);
    if (game_over && !client->msg_head) {
        remove_client(&client);
    }
}

void check_if_all_messages_sent(fd_set *readset, fd_set *writeset) {
    if (!game_over)
        return;
    for (Client *cur = client_list; cur != NULL; cur = cur->next)
        if (cur->fd > 0 && cur->msg_head)
            return;
    // All messages have been sent.
    for (Client *cur = client_list; cur != NULL; cur = cur->next) {
        if (cur->fd > 0) {
            close(cur->fd);
            FD_CLR(cur->fd, readset);
            FD_CLR(cur->fd, writeset);
            cur->fd = 0;
        }
    }
    free_all_clients();
    reset_game();
}

// ----- Main Function -----
int main(int argc, char *argv[]) {
    int port_arg, seed;
    parse_arguments(argc, argv, &port_arg, &seed, &max_players);
    signal(SIGINT, cleanup_and_exit);
    srand(seed);
    reset_game();

    listen_fd = create_listening_socket(port_arg);
    printf("Server listening on port %d...\n", port_arg);

    max_fd = listen_fd;
    client_list = NULL;
    len_client_list = 0;

    fd_set tmp_read, tmp_write;
    FD_ZERO(&readset);
    FD_ZERO(&writeset);
    FD_SET(listen_fd, &readset);

    while (1) {
        tmp_read = readset;
        tmp_write = writeset;
        int activity = select(max_fd + 1, &tmp_read, &tmp_write, NULL, NULL);
        if (activity < 0 && errno != EINTR) {
            perror("select");
            cleanup_and_exit(-1);
        }
        if (FD_ISSET(listen_fd, &tmp_read)) {
            printf("Server is ready to read from welcome socket %d\n", listen_fd);
            accept_new_client();
        }
        Client *cur = client_list;
        for (cur = client_list; cur != NULL; cur = cur->next) {
            if (cur->fd > 0 && FD_ISSET(cur->fd, &tmp_read)) {
                printf("Server is ready to read from player %d on socket %d\n", cur->id, cur->fd);
                read_from_client(&cur);
            }

            if (cur && cur->fd > 0 && FD_ISSET(cur->fd, &tmp_write)) {
                printf("Server is ready to write to player %d on socket %d\n", cur->id, cur->fd);
                write_to_client(cur);
            }
        }

        // check_if_all_messages_sent(&readset, &writeset);
        // If maximum players are connected, remove listen_fd from read set.
        if (len_client_list >= max_players)
            FD_CLR(listen_fd, &readset);
        else
            FD_SET(listen_fd, &readset);

        if (game_over && len_client_list == 0) {
            target_number = (rand() % 100) + 1;
            game_over = 0;
            max_fd = listen_fd;
        }
        // if (len_client_list == 1)
        //    cleanup_and_exit(10);
    }
    cleanup_and_exit(0);
    return 0;
}