#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

#include <pthread.h>

#include <sys/select.h>

#include "./hash_table.c"

#define PORT 8080

typedef struct {
    int client_socket;
    int dest_socket;
} ClientThreadArgs;

HashTable *global_cache;

void cut_request(char *uri_start, char *path_start) {
    char *dest = uri_start;
    for (char *src = path_start; *src != '\0'; dest++,src++) {
        *dest = *src;
    }
    *dest = '\0';
}

void parsenformat_request(char *request, char *out_url, char **out_port) {
    char *uri_start = strchr(request, ' ') + 1;
    char *uri_end = strchr(uri_start, ' ');
    // for searching in uri only
    *uri_end = '\0';
    int uri_len = uri_end - uri_start;
    // protocol
    char *proto_end = strstr(uri_start, "://"), *url_start = NULL;
    if (proto_end == NULL) {
        url_start = uri_start;
    } else {
        url_start = proto_end + 3;
    }
    // path
    char *path_start = strchr(url_start, '/');
    char *url_end = path_start;
    if (path_start == NULL) {
        url_end = uri_end;
    }
    int url_len = url_end - url_start;
    // copy with port
    strncpy(out_url, url_start, url_len);
    *(out_url + url_len) = '\0';
    char *port_start = strchr(out_url, ':');
    if (port_start == NULL) {
        if (proto_end != NULL && strncmp(uri_start, "http", 4) == 0) {
            *out_port = "80";
        } else {
            *out_port = NULL;
        }
    } else {
        *out_port = port_start + 1;
        *port_start = '\0';
    }
    // replace uri with path
    *uri_end = ' ';
    if (path_start == NULL) {
        // strcpy(uri_start + 1, uri_end);
        cut_request(uri_start + 1, uri_end);
        *uri_start = '*';
    } else {
        // strcpy(uri_start, path_start);
        cut_request(uri_start, path_start);
    }
    return NULL;
}

int close_client(ClientThreadArgs *arg) {
    int rt = 0;
    if (arg->client_socket) {
        rt &= close(arg->client_socket);
    }
    if (arg->dest_socket) {
        rt &= close(arg->dest_socket);
    }
    free(arg);
    pthread_exit(NULL);
    return rt;
}

void get_ht_key(char *request, char *out_key) {
    char *uri_start = strchr(request, ' ') + 1;
    char *uri_end = strchr(uri_start, ' ');
    strncpy(out_key, uri_start, uri_end - uri_start);
}

int handle_client(void *arg) {
    ClientThreadArgs *args = (ClientThreadArgs *) arg;

    // Read the HTTP request from the client.
    char request[1024];
    ssize_t request_len = recv(args->client_socket, request, sizeof(request) - 1, 0);
    if (request_len <= 0) {
        perror("read");
        return close_client(args);
    }
    // puts(request);
    // puts("<<<<<<<<<");
    
    char ht_key[128];
    get_ht_key(request, ht_key);

    char method[32];
    strncpy(method, request, strchr(request, ' ') - request);

    // Parse the HTTP request to extract the request URL.
    char url[128], *port;
    parsenformat_request(request, url, &port);

    char *cached = ht_get(global_cache, ht_key);

    if (cached != NULL) {
        send(args->client_socket, cached, strlen(cached), 0);
        return close_client(args);
    }

    // Forward the HTTP request to the destination server.
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    // hints.ai_family = AF_INET;
    // hints.ai_socktype = SOCK_STREAM;

    // printf("resolving %s:%s\n", url, port);
    if (getaddrinfo(url, port, NULL, &res) != 0) {
        perror("Failed to resolve address");
        return close_client(args);
    }

    if ((args->dest_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) <= 0) {
        perror("socket");
        return close_client(args);
    }

    if (connect(args->dest_socket, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        return close_client(args);
    }

    if (strcmp(method, "CONNECT") == 0) {
        const char response[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (send(args->client_socket, response, sizeof(response), 0) <= 0) {
            perror("connect ok");
            return close_client(args);
        }
    } else {
        // Write the HTTP request to the destination server.
        // puts(request);
        // puts(">>>>>>>>>");
        if (send(args->dest_socket, request, request_len, 0) <= 0) {
            perror("send");
            return close_client(args);
        }
    }

    int is_first_request = 1;
    int max_fd = (args->dest_socket < args->client_socket) ? args->client_socket : args->dest_socket ;
    fd_set read_fds;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(args->dest_socket, &read_fds);
        FD_SET(args->client_socket, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;

        int ready_fds = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready_fds < 0) {
            perror("select");
            return close_client(args);
        }
        if (ready_fds > 0) {
            char response[1024];
            if (FD_ISSET(args->client_socket, &read_fds)) {
                ssize_t response_len = recv(args->client_socket, response, sizeof(response) - 1, 0);
                is_first_request = 0;
                if (response_len <= 0) {
                    break;
                }
                if (send(args->client_socket, response, response_len, 0) <= 0) {
                    break;
                }
            }
            if (FD_ISSET(args->dest_socket, &read_fds)) {
                ssize_t response_len = recv(args->dest_socket, response, sizeof(response) - 1, 0);
                if (response_len <= 0) {
                    // perror("read");
                    break;
                }
                if (is_first_request) {
                    ht_set(global_cache, ht_key, response);
                }
                // Forward the HTTP response to the client.
                if (send(args->client_socket, response, response_len, 0) <= 0) {
                    // perror("forward");
                    break;
                }
            }
        } else {
            break;
        }
    }
    
    close_client(args);
}

int main() {
    // Create a global cache.
    global_cache = ht_create(150);

    // Create a server socket to listen for incoming client connections.
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        exit(1);
    }
    int reuse = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        perror("Reuse");
        exit(1);
    }

    // Set up the server address.
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;

    // Bind the server socket to the server address.
    if (bind(server_socket, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
        perror("bind");
        exit(1);
    }

    // Listen for incoming client connections.
    if (listen(server_socket, 2) < 0) {
        perror("listen");
        exit(1);
    }

    // Accept incoming client connections.
    struct sockaddr_in client_address;
    socklen_t client_address_len = sizeof(client_address);
    int client_socket;

    while ((client_socket = accept(server_socket, (struct sockaddr *) &client_address, &client_address_len)) >= 0) {
        // Create a new thread to handle the client connection.
        ClientThreadArgs *args = malloc(sizeof(ClientThreadArgs));
        args->client_socket = client_socket;
        args->dest_socket = 0;

        // handle_client(args);
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, args);

        pthread_detach(thread);
    }

    // Close the server socket.
    close(server_socket);

    // Destroy the global cache.
    ht_destroy(global_cache);

    return 0;
}