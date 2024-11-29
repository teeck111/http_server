#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>

#define BUFFSIZE 8192
#define MAXTHREADS 100
#define DOCROOT "./www"
#define TIMEOUT 10

// Function prototypes
void *handle_client(void *arg);
void send_error(int client_socket, int status_code, const char *message);
const char *get_mime(const char *path);
void send_headers(int client_socket, int status_code, const char *status_text, const char *mime_type, long content_length, const char *connection);
void parse_request(const char *request, char *method, char *url, char *version);
int is_directory(const char *path);
int check_keep_alive(const char *request);
void send_file(int client_socket, const char *path, const char *mime_type, int keep_alive);
void send_directory_listing(int client_socket, const char *path, int keep_alive);

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Create a TCP socket
    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Setup the server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind the socket to the port
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_socket, 10) == -1)
    {
        perror("listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", port);

    // Multithreading with pthread
    pthread_t threads[MAXTHREADS];
    int thread_count = 0;

    while (1)
    {
        // Accept a new client connection
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == -1)
        {
            perror("accept");
            continue;
        }

        printf("---------------------\n");
        printf("Client connected...\n");
        printf("---------------------\n");

        // Allocate memory for socket descriptor to pass to thread
        int *new_sock = malloc(sizeof(int));
        if (new_sock == NULL)
        {
            perror("malloc");
            close(client_socket);
            continue;
        }
        *new_sock = client_socket;

        // Handle the client connection in a new thread
        if (pthread_create(&threads[thread_count++], NULL, handle_client, (void *)new_sock) != 0)
        {
            perror("pthread_create");
            close(client_socket);
            free(new_sock);
        }

        // Reap completed threads to avoid overflow
        if (thread_count >= MAXTHREADS)
        {
            for (int i = 0; i < MAXTHREADS; i++)
            {
                pthread_join(threads[i], NULL);
            }
            thread_count = 0;
        }
    }

    // Close the server socket
    close(server_socket);
    return 0;
}

void *handle_client(void *arg)
{
    int client_socket = *(int *)arg;
    free(arg); // Free the memory for the socket descriptor
    char buffer[BUFFSIZE];
    char method[10], url[256], version[10];
    ssize_t bytes_received;
    int keep_alive = 0;

    struct timeval timeout;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;

    fd_set readfds; // file descriptor set to monitor socket for incoming data
    int activity;

    do
    {
        // Reset and add client socket to readfds for monitoring
        FD_ZERO(&readfds);
        FD_SET(client_socket, &readfds);

        // Check for incoming data or timeout
        activity = select(client_socket + 1, &readfds, NULL, NULL, &timeout);
        if (activity == 0)
        {
            // Timeout occurred without activity, close connection
            printf("Keep-alive timeout reached. Closing connection.\n");
            break;
        }
        else if (activity < 0)
        {
            perror("select");
            break;
        }

        // Receive request if data is available
        if (FD_ISSET(client_socket, &readfds))
        {
            bytes_received = recv(client_socket, buffer, BUFFSIZE - 1, 0);
            if (bytes_received <= 0)
            {
                close(client_socket);
                return NULL;
            }

            buffer[bytes_received] = '\0';
            printf("Received request:\n%s\n", buffer);

            parse_request(buffer, method, url, version);

            // Validate HTTP version
            if (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0)
            {
                send_error(client_socket, 505, "HTTP Version Not Supported");
                break;
            }

            // Validate request method
            if (strcmp(method, "GET") != 0)
            {
                send_error(client_socket, 405, "Method Not Allowed");
                break;
            }

            if (strlen(url) == 0 || url[0] != '/')
            {
                send_error(client_socket, 400, "Bad Request");
                break;
            }

            keep_alive = check_keep_alive(buffer);

            if (keep_alive)
            {
                timeout.tv_sec = TIMEOUT;
                timeout.tv_usec = 0;
            }

            // Construct the file path
            char path[512];
            snprintf(path, sizeof(path), "%s%s", DOCROOT, url);

            // Serve the requested file or directory listing
            if (is_directory(path))
            {
                char index_html[512];
                snprintf(index_html, sizeof(index_html), "%s/index.html", path);
                if (access(index_html, F_OK) != -1)
                {
                    send_file(client_socket, index_html, "text/html", keep_alive);
                }
                else
                {
                    send_directory_listing(client_socket, path, keep_alive);
                }
            }
            else if (access(path, F_OK) != -1)
            {
                send_file(client_socket, path, get_mime(path), keep_alive);
            }
            else
            {
                send_error(client_socket, 404, "Not Found");
            }

            // Reset timeout for next request in keep-alive mode
            if (keep_alive)
            {
                timeout.tv_sec = TIMEOUT;
                timeout.tv_usec = 0;
            }
            else
            {
                printf("Connection close requested by client\n\n");
                break; // Break loop if no keep-alive requested
            }
        }
    } while (keep_alive);

    close(client_socket);
    printf("\nConnection Closed\n\n");
    return NULL;
}

void send_headers(int client_socket, int status_code, const char *status_text, const char *mime_type, long content_length, const char *connection)
{
    char headers[BUFFSIZE];
    // formats the header buffer
    snprintf(headers, sizeof(headers),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: %s\r\n\r\n",
             status_code, status_text, mime_type, content_length, connection);
    printf("Sending response headers:\n%s", headers); // Print the response headers
    printf(" ---------------------------- \n \n");
    send(client_socket, headers, strlen(headers), 0);
}


// Function to check if keep-alive is requested or connection should be closed
int check_keep_alive(const char *request)
{
    // To make sure it is case insensitive
    const char *close_keywords[] = {"Connection: close", "connection: close", "Connection: Close", "connection: Close"};
    const char *keep_alive_keywords[] = {"Connection: keep-alive", "Connection: Keep-alive", "connection: keep-alive", "connection: Keep-alive"};

    for (int i = 0; i < 4; i++)
    {
        if (strstr(request, close_keywords[i]) != NULL)
            return 0; // Explicitly close connection
        if (strstr(request, keep_alive_keywords[i]) != NULL)
            return 1; // Explicitly keep connection alive
    }

    return 0; // Default to closing connection if no header is found
}

void send_error(int client_socket, int status_code, const char *message)
{
    char response[BUFFSIZE];
    snprintf(response, sizeof(response), "HTTP/1.1 %d %s\r\nContent-Length: %ld\r\nContent-Type: text/plain\r\n\r\n%s",
             status_code, message, strlen(message), message);
    send(client_socket, response, strlen(response), 0);
}

void send_file(int client_socket, const char *path, const char *mime_type, int keep_alive)
{
    // Check if the file is readable
    if (access(path, R_OK) == -1)
    {
        send_error(client_socket, 403, "Forbidden");
        return;
    }

    FILE *file = fopen(path, "rb");
    // If file doesn't exist
    if (!file)
    {
        send_error(client_socket, 404, "Not Found"); 
        return;
    }

    // Getting the file size by seeking to the end
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    // If keep alive is true, connection = keep-alive, if not it chooses close
    const char *connection = keep_alive ? "keep-alive" : "close";
    send_headers(client_socket, 200, "OK", mime_type, file_size, connection);

    char buffer[BUFFSIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFSIZE, file)) > 0)
    {
        send(client_socket, buffer, bytes_read, 0);
    }
    fclose(file);
}
const char *get_mime(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL)
        // Generic mime type that indicates binary data
        return "application/octet-stream"; 
    if (strcmp(ext, ".html") == 0)
        return "text/html";
    if (strcmp(ext, ".txt") == 0)
        return "text/plain";
    if (strcmp(ext, ".png") == 0)
        return "image/png";
    if (strcmp(ext, ".gif") == 0)
        return "image/gif";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpg";
    if (strcmp(ext, ".ico") == 0)
        return "image/x-icon";
    if (strcmp(ext, ".css") == 0)
        return "text/css";
    if (strcmp(ext, ".js") == 0)
        return "application/javascript";
    if (strcmp(ext, ".webp") == 0)
    return "image/webp";
    if (strcmp(ext, ".avif") == 0)
        return "image/avif";
return "application/octet-stream";
}


void parse_request(const char *request, char *method, char *url, char *version)
{
    sscanf(request, "%s %s %s", method, url, version);
}

int is_directory(const char *path)
{
    struct stat statbuf;
    if (stat(path, &statbuf) != 0)
        return 0;
    return S_ISDIR(statbuf.st_mode);
}

void send_directory_listing(int client_socket, const char *path, int keep_alive)
{
    char response[BUFFSIZE];
    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: %s\r\n\r\n", keep_alive ? "keep-alive" : "close");
    send(client_socket, response, strlen(response), 0);

    snprintf(response, sizeof(response), "<html><body><h1>Directory listing for %s</h1><ul>", path);
    send(client_socket, response, strlen(response), 0);

    DIR *dir = opendir(path);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            snprintf(response, sizeof(response), "<li><a href=\"%s/%s\">%s</a></li>", path, entry->d_name, entry->d_name);
            send(client_socket, response, strlen(response), 0);
        }
        closedir(dir);
    }

    snprintf(response, sizeof(response), "</ul></body></html>");
    send(client_socket, response, strlen(response), 0);
}