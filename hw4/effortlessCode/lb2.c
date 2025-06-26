#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define BUFFER_SIZE 1024
#define MAX_SERVERS 3
#define MAX_CLIENTS 5
#define MIN_DURATION 1
#define MAX_DURATION 9

typedef struct {
    int socket;
    char ip[16];
    int port;
    char type[10]; // "VIDEO" or "MUSIC"
    double expected_time;
    double last_update;
} ServerInfo;

typedef struct {
    int client_socket;
    int server_socket;
    char request[10];
    char server_ip[16];
    int server_port;
} ThreadArgs;

ServerInfo servers[MAX_SERVERS];
pthread_mutex_t lock;

// Function prototypes
void* handle_request(void* args);
int connect_to_server(const char* ip, int port);
void greedy_balance(char* request, int* chosen_server, double* expected_time);
double get_current_time();
void validate_request(const char* request, int* duration, int* is_valid);

int main() {
    const char* server_ips[MAX_SERVERS] = {"192.168.0.101", "192.168.0.102", "192.168.0.103"};
    const char* server_types[MAX_SERVERS] = {"VIDEO", "VIDEO", "MUSIC"};
    int server_port = 80;

    pthread_mutex_init(&lock, NULL);

    // Initialize server connections
    for (int i = 0; i < MAX_SERVERS; i++) {
        servers[i].socket = connect_to_server(server_ips[i], server_port);
        if (servers[i].socket < 0) {
            fprintf(stderr, "[ERROR] Failed to connect to server %d (%s)\n", i, server_ips[i]);
            exit(1);
        }
        strcpy(servers[i].ip, server_ips[i]);
        servers[i].port = server_port;
        strcpy(servers[i].type, server_types[i]);
        servers[i].expected_time = 0.0;
        servers[i].last_update = get_current_time();
        printf("[INFO] Connected to server %d (%s) at %s\n", i, server_types[i], server_ips[i]);
    }

    // Setup load balancer listener socket
    int lb_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (lb_socket < 0) {
        perror("[ERROR] Socket creation failed");
        exit(1);
    }

    // Allow socket reuse
    int opt = 1;
    if (setsockopt(lb_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("[ERROR] Setsockopt failed");
        close(lb_socket);
        exit(1);
    }

    struct sockaddr_in lb_addr;
    memset(&lb_addr, 0, sizeof(lb_addr));
    lb_addr.sin_family = AF_INET;
    lb_addr.sin_addr.s_addr = inet_addr("10.0.0.1");
    lb_addr.sin_port = htons(80);

    if (bind(lb_socket, (struct sockaddr*)&lb_addr, sizeof(lb_addr))) {
        perror("[ERROR] Bind failed");
        close(lb_socket);
        exit(1);
    }

    if (listen(lb_socket, MAX_CLIENTS)) {
        perror("[ERROR] Listen failed");
        close(lb_socket);
        exit(1);
    }

    printf("[INFO] Load Balancer listening on 10.0.0.1:80\n");

    // Main accept loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(lb_socket, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            perror("[ERROR] Accept failed");
            continue;
        }

        // Get client IP for logging
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        // Receive request
        char request[BUFFER_SIZE];
        int bytes_received = recv(client_socket, request, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            perror("[ERROR] Invalid request");
            close(client_socket);
            continue;
        }
        request[bytes_received] = '\0';

        // Validate request
        int duration, is_valid;
        validate_request(request, &duration, &is_valid);
        if (!is_valid) {
            fprintf(stderr, "[WARNING] Invalid request from %s: %s\n", client_ip, request);
            close(client_socket);
            continue;
        }

        printf("[CLIENT] %s sent request: %s\n", client_ip, request);

        // Choose server and delegate request
        int chosen_server;
        double expected_time;
        greedy_balance(request, &chosen_server, &expected_time);

        printf("[LB] Delegated %s to server %d (%s), expected time: %.2fs\n",
               request, chosen_server, servers[chosen_server].type, expected_time);

        // Create thread to handle request
        ThreadArgs* args = malloc(sizeof(ThreadArgs));
        if (!args) {
            perror("[ERROR] Memory allocation failed");
            close(client_socket);
            continue;
        }

        args->client_socket = client_socket;
        args->server_socket = servers[chosen_server].socket;
        strncpy(args->request, request, sizeof(args->request));
        strncpy(args->server_ip, servers[chosen_server].ip, sizeof(args->server_ip));
        args->server_port = servers[chosen_server].port;

        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_request, args)) {
            perror("[ERROR] Thread creation failed");
            free(args);
            close(client_socket);
        } else {
            pthread_detach(thread);
        }
    }

    // Cleanup (theoretically unreachable)
    pthread_mutex_destroy(&lock);
    close(lb_socket);
    return 0;
}

void* handle_request(void* args) {
    ThreadArgs* targs = (ThreadArgs*)args;
    char buffer[BUFFER_SIZE];

    // Send request to server
    if (send(targs->server_socket, targs->request, strlen(targs->request), 0) < 0) {
        perror("[ERROR] Failed to send to server");
        goto cleanup;
    }

    // Receive response from server
    int bytes_received = recv(targs->server_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received <= 0) {
        perror("[ERROR] Server disconnected, attempting reconnect...");
        close(targs->server_socket);
        
        // Reconnect and retry once
        targs->server_socket = connect_to_server(targs->server_ip, targs->server_port);
        if (targs->server_socket < 0 || 
            send(targs->server_socket, targs->request, strlen(targs->request), 0) < 0 || 
            (bytes_received = recv(targs->server_socket, buffer, BUFFER_SIZE - 1, 0)) <= 0) {
            perror("[CRITICAL] Reconnect failed");
            goto cleanup;
        }
    }

    buffer[bytes_received] = '\0';
    printf("[SERVER] Response: %s\n", buffer);

    // Send response back to client
    if (send(targs->client_socket, buffer, bytes_received, 0) < 0) {
        perror("[ERROR] Failed to send to client");
    }

cleanup:
    close(targs->client_socket);
    free(targs);
    return NULL;
}

void greedy_balance(char* request, int* chosen_server, double* expected_time) {
    pthread_mutex_lock(&lock);
    double now = get_current_time();

    // Update expected_time for all servers
    for (int i = 0; i < MAX_SERVERS; i++) {
        double delta = now - servers[i].last_update;
        servers[i].expected_time = (servers[i].expected_time > delta) ? 
                                 servers[i].expected_time - delta : 0;
        servers[i].last_update = now;
    }

    char req_type = request[0];
    int duration = request[1] - '0';
    double min_time = -1;

    // Calculate weighted times for each server
    for (int i = 0; i < MAX_SERVERS; i++) {
        double weight;
        if (strcmp(servers[i].type, "VIDEO") == 0) {
            weight = (req_type == 'M') ? 2.0 : 1.0;  // Video servers: M=2x, V/P=1x
        } else {  // MUSIC server
            if (req_type == 'V') weight = 3.0;
            else if (req_type == 'P') weight = 2.0;
            else weight = 1.0;  // M
        }

        double total_time = servers[i].expected_time + (duration * weight);
        if (min_time < 0 || total_time < min_time) {
            min_time = total_time;
            *chosen_server = i;
            *expected_time = total_time;
        }
    }

    // Update chosen server's expected time
    servers[*chosen_server].expected_time = *expected_time;
    servers[*chosen_server].last_update = now;
    pthread_mutex_unlock(&lock);
}

int connect_to_server(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) {
        close(sock);
        return -1;
    }
    return sock;
}

double get_current_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void validate_request(const char* request, int* duration, int* is_valid) {
    *is_valid = 0;
    
    if (strlen(request) < 2) {
        return;
    }

    char req_type = request[0];
    *duration = request[1] - '0';

    // Validate request type
    if (req_type != 'M' && req_type != 'V' && req_type != 'P') {
        return;
    }

    // Validate duration
    if (*duration < MIN_DURATION || *duration > MAX_DURATION) {
        return;
    }

    *is_valid = 1;
}