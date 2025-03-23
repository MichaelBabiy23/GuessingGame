#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>

#define BUFFER_SIZE 1024
#define DEBUG 0

#if DEBUG
  #define DEBUG_PRINT(fmt, ...) fprintf(stderr, "DEBUG: " fmt, ##__VA_ARGS__)
#else
  #define DEBUG_PRINT(fmt, ...) do {} while(0)
#endif

// Structure to represent a message node in the queue (one complete line)
typedef struct MessageNode {
    char message[BUFFER_SIZE];
    struct MessageNode *next;
} MessageNode;

// Structure to represent a client
typedef struct {
    int fd;                 // Client socket descriptor
    int id;                 // Client ID (1 to max_players)
    MessageNode *queue;     // Message queue head
    MessageNode *queue_tail;// Message queue tail
    int printed_write;      // Flag: 0 if "ready to write" not printed for current flush cycle, 1 if printed
} Client;

// Global variables
int max_players, listen_fd, max_fd, target_num, client_count = 0, game_over = 0;
Client* clients;

// Function prototypes – using your function names:
int isInteger(const char *str);
int enqueue_message(Client *client, const char *message);
char *dequeue_message(Client *client);
void free_message_queue(Client* client);
void free_clients();
int broadcast_message(Client* clients, const char *message, fd_set *set_write, int max_fd, int current_client);
void cleanup_and_exit(int signum);
void accept_new_client(int *client_count, int *max_fd, fd_set *set_read, fd_set *set_write);
void handle_client_input(int i, fd_set *set_read, fd_set *set_write);
void send_message(int i, fd_set *set_read, fd_set *set_write);
void update_max_fd();

// Check if a string is a valid integer
int isInteger(const char *str) {
    char *endptr;
    long value = strtol(str, &endptr, 10);
    if (*endptr != '\0')
        return 0;
    if (value < INT_MIN || value > INT_MAX)
        return 0;
    return 1;
}

// Enqueue a message (one complete line) for a client
int enqueue_message(Client *client, const char *message) {
    MessageNode *new_node = (MessageNode *)malloc(sizeof(MessageNode));
    if (!new_node) {
        perror("Failed to allocate memory for message");
        return -1;
    }
    strncpy(new_node->message, message, BUFFER_SIZE - 1);
    new_node->message[BUFFER_SIZE - 1] = '\0';
    new_node->next = NULL;
    // Reset printed_write flag if queue was empty
    if (!client->queue)
        client->printed_write = 0;
    if (!client->queue) {
        client->queue = new_node;
        client->queue_tail = new_node;
    } else {
        client->queue_tail->next = new_node;
        client->queue_tail = new_node;
    }
    return 0;
}

// Dequeue a message from a client's queue
char *dequeue_message(Client *client) {
    if (!client->queue)
        return NULL;
    MessageNode *node = client->queue;
    char *msg = strdup(node->message);
    client->queue = node->next;
    if (!client->queue)
        client->queue_tail = NULL;
    free(node);
    // Reset printed_write flag when queue becomes empty.
    if (!client->queue)
        client->printed_write = 0;
    return msg;
}

// Free a client's message queue
void free_message_queue(Client* client) {
    MessageNode *current = client->queue;
    MessageNode *next;
    while (current != NULL) {
        next = current->next;
        free(current);
        current = next;
    }
    client->queue = client->queue_tail = NULL;
    client->printed_write = 0;
}

// Free all clients and close their sockets
void free_clients() {
    for (int i = 0; i < max_players; i++) {
        if (clients[i].fd != 0) {
            close(clients[i].fd);
            free_message_queue(&clients[i]);
        }
    }
}

// Broadcast a message to all clients; if current_client != -1, skip that index
int broadcast_message(Client* clients, const char *message, fd_set *set_write, int max_fd, int current_client) {
    for (int j = 0; j < max_players; j++) {
        if (clients[j].fd != 0) {
            if (current_client == -1 || j != current_client) {
                if (enqueue_message(&clients[j], message) < 0)
                    return -1;
                FD_SET(clients[j].fd, set_write);
            }
        }
    }
    return 0;
}

// Clean up resources and exit
void cleanup_and_exit(int signum) {
    free_clients();
    free(clients);
    if (listen_fd > 0)
        close(listen_fd);
    exit(signum == -1 ? EXIT_FAILURE : EXIT_SUCCESS);
}

// Accept a new client connection
void accept_new_client(int *client_count, int *max_fd, fd_set *set_read, fd_set *set_write) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int new_socket = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (new_socket < 0) {
        perror("accept");
        cleanup_and_exit(-1);
    }
    // Set new socket non-blocking
    int flags = fcntl(new_socket, F_GETFL, 0);
    fcntl(new_socket, F_SETFL, flags | O_NONBLOCK);
    char buffer[BUFFER_SIZE];
    // Find an empty slot in clients array
    for (int i = 0; i < max_players; i++) {
        if (clients[i].fd == 0) {
            clients[i].fd = new_socket;
            clients[i].id = i + 1;
            clients[i].printed_write = 0;
            FD_SET(new_socket, set_read);
            if (new_socket > *max_fd)
                *max_fd = new_socket;
            (*client_count)++;
            sprintf(buffer, "Welcome to the game, your id is %d\n", clients[i].id);
            if (enqueue_message(&clients[i], buffer) < 0)
                cleanup_and_exit(-1);
            // Immediately send the welcome message (flush one line)
            send_message(i, set_read, set_write);
            FD_SET(new_socket, set_write);
            sprintf(buffer, "Player %d joined the game\n", clients[i].id);
            if (broadcast_message(clients, buffer, set_write, *max_fd, i) < 0)
                cleanup_and_exit(-1);
            break;
        }
    }
}

// Handle client input: read data, check for "quit", or process guess and broadcast responses.
void handle_client_input(int i, fd_set *set_read, fd_set *set_write) {
    char buffer[BUFFER_SIZE];
    char num_buffer[20];
    int bytes_read = read(clients[i].fd, num_buffer, sizeof(num_buffer) - 1);
    if (bytes_read <= 0) {
        // Client disconnected
        close(clients[i].fd);
        FD_CLR(clients[i].fd, set_read);
        FD_CLR(clients[i].fd, set_write);
        free_message_queue(&clients[i]);
        client_count--;
        sprintf(buffer, "Player %d disconnected\n", clients[i].id);
        broadcast_message(clients, buffer, set_write, max_fd, i);
        clients[i].id = 0;
        if (clients[i].fd == max_fd)
            update_max_fd();
        clients[i].fd = 0;
    } else {
        num_buffer[bytes_read] = '\0';
        // Check if client sends "quit"
        if (strncmp(num_buffer, "quit", 4) == 0) {
            close(clients[i].fd);
            FD_CLR(clients[i].fd, set_read);
            FD_CLR(clients[i].fd, set_write);
            free_message_queue(&clients[i]);
            client_count--;
            sprintf(buffer, "Player %d disconnected\n", clients[i].id);
            broadcast_message(clients, buffer, set_write, max_fd, i);
            clients[i].id = 0;
            if (clients[i].fd == max_fd)
                update_max_fd();
            clients[i].fd = 0;
            return;
        }
        int num = atoi(num_buffer);
        sprintf(buffer, "Player %d guessed %d\n", clients[i].id, num);
        broadcast_message(clients, buffer, set_write, max_fd, -1);
        if (num < target_num)
            sprintf(buffer, "The guess %d is too low\n", num);
        else if (num > target_num)
            sprintf(buffer, "The guess %d is too high\n", num);
        else {
            sprintf(buffer, "Player %d wins\n", clients[i].id);
            broadcast_message(clients, buffer, set_write, max_fd, -1);
            sprintf(buffer, "The correct guessing is %d\n", num);
            broadcast_message(clients, buffer, set_write, max_fd, -1);
            game_over = 1;
        }
    }
}

// Send one queued message (one line) from the client's queue.
// This function sends exactly one line per call.
void send_message(int i, fd_set *set_read, fd_set *set_write) {
    char *message = dequeue_message(&clients[i]);
    if (message) {
        write(clients[i].fd, message, strlen(message));
        free(message);
    }
    // Reset printed_write flag if queue becomes empty
    if (!clients[i].queue)
        clients[i].printed_write = 0;
    // If game_over is set and queue is empty, disconnect this client.
    if (!clients[i].queue && game_over) {
        FD_CLR(clients[i].fd, set_write);
        close(clients[i].fd);
        client_count--;
        FD_CLR(clients[i].fd, set_read);
        FD_CLR(clients[i].fd, set_write);
        clients[i].id = 0;
        if (clients[i].fd == max_fd)
            update_max_fd();
        clients[i].fd = 0;
        clients[i].printed_write = 0;
    }
}

// Update the maximum file descriptor among all clients
void update_max_fd() {
    max_fd = listen_fd;
    for (int i = 0; i < max_players; i++) {
        if (clients[i].fd > max_fd)
            max_fd = clients[i].fd;
    }
}

int main(int argc, char *argv[]) {
    // Validate command-line arguments
    if (argc != 4) {
        printf("Usage: ./server <port> <seed> <max-number-of-players>\n");
        exit(EXIT_FAILURE);
    }
    if (!isInteger(argv[1]) || !isInteger(argv[2]) || !isInteger(argv[3])) {
        printf("Usage: ./server <port> <seed> <max-number-of-players>\n");
        exit(EXIT_FAILURE);
    }
    int port_num = atoi(argv[1]);
    int seed = atoi(argv[2]);
    max_players = atoi(argv[3]);

    // Register SIGINT handler
    signal(SIGINT, cleanup_and_exit);

    srand(seed);
    target_num = (rand() % 100) + 1;

    struct sockaddr_in srv;
    if ((listen_fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port_num);
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr *) &srv, sizeof(srv)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // Allocate memory for clients array
    clients = (Client *) malloc(max_players * sizeof(Client));
    if (!clients) {
        perror("malloc");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    memset(clients, 0, max_players * sizeof(Client));

    int activity;
    fd_set set_read, set_write;

    while (1) {
        FD_ZERO(&set_read);
        FD_ZERO(&set_write);

        // Add the listen socket if there is room
        if (client_count < max_players)
            FD_SET(listen_fd, &set_read);
        max_fd = listen_fd;

        // Rebuild FD sets from current clients array
        for (int i = 0; i < max_players; i++) {
            if (clients[i].fd != 0) {
                FD_SET(clients[i].fd, &set_read);
                if (clients[i].queue)
                    FD_SET(clients[i].fd, &set_write);
                if (clients[i].fd > max_fd)
                    max_fd = clients[i].fd;
            }
        }

        activity = select(max_fd + 1, &set_read, &set_write, NULL, NULL);
        if (activity < 0) {
            perror("select");
            cleanup_and_exit(-1);
        }

        // Handle new connection on welcome socket
        if (FD_ISSET(listen_fd, &set_read)) {
            printf("Server is ready to read from welcome socket %d\n", listen_fd);
            accept_new_client(&client_count, &max_fd, &set_read, &set_write);
            activity--;
        }

        // Process I/O for each client
        for (int i = 0; i < max_players && activity > 0; i++) {
            if (clients[i].fd != 0) {
                if (FD_ISSET(clients[i].fd, &set_read)) {
                    printf("Server is ready to read from player %d on socket %d\n", clients[i].id, clients[i].fd);
                    handle_client_input(i, &set_read, &set_write);
                    activity--;
                }
                if (FD_ISSET(clients[i].fd, &set_write) && clients[i].queue) {
                    if (clients[i].printed_write == 0) {
                        printf("Server is ready to write to player %d on socket %d\n", clients[i].id, clients[i].fd);
                        clients[i].printed_write = 1;
                    }
                    send_message(i, &set_read, &set_write);
                    activity--;
                }
            }
        }

        // If game_over is set and no clients remain, start a new game round.
        if (game_over && client_count == 0) {
            target_num = (rand() % 100) + 1;
            game_over = 0;
            max_fd = listen_fd;
            printf("New game started. New target generated.\n");
        }
    }

    cleanup_and_exit(0);
    return 0;
}
