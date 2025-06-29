/*
A multi-user chat server
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <cjson/cJSON.h>

//#include <libwebsockets.h>

#define PORT 8080 //Port we're listening on/my port
#define SERVER_MAX 50


/*
Convert socket to IP address string.
addr: struct sockaddr_in or struct sockaddr_in6
*/
const char *inet_ntop2(void *addr, char *buf, size_t size) {
    struct sockaddr_storage *sas = addr;
    struct sockaddr_in *sa4;
    struct sockaddr_in6 *sa6;
    void *src;

    switch(sas ->ss_family) {
        case AF_INET:
            sa4 = addr;
            src = &(sa4->sin_addr);
            break;
        case AF_INET6:
            sa6 = addr;
            src = &(sa6->sin6_addr);
            break;
        default:
            return NULL;
    }
    return inet_ntop(sas->ss_family, src, buf, size);
}

/*
 * Return a listening socket.
 */

 int get_listener_socket(void) {
    int listener, queue = 10, yes = 1;
    struct sockaddr_in server_addr;
    char hostname[100];

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        fprintf(stderr, "webserver: Failed to make listener socket");
    }

    memset(&server_addr, 0, sizeof server_addr);
    server_addr.sin_family = AF_INET;
    int server_addr_len = sizeof(server_addr);
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
    if (bind(listener,(struct sockaddr *)&server_addr,server_addr_len) != 0) {
        perror("webserver: failed to bind()");
        close(listener);
        exit(1);
    }
    
    if (listen(listener, queue) == -1) {
        return -1;
    }

    return listener;

 }

 void add_to_fds(struct pollfd **fds, int newfd, int *fd_count, int *fd_size) {
    // Making sure the there is enough room in fds for the newfd.
    if (*fd_count == SERVER_MAX || *fd_size == SERVER_MAX) {
        printf("At server capacity.");
        return;
    }
    if (*fd_count == *fd_size) {
        *fd_size *= 2; // making fd_size twice as much

        *fds = realloc(*fds,sizeof(**fds) * *fd_size); //reallocating twice as much space for the array
    }

    (*fds)[*fd_count].fd = newfd;
    (*fds)[*fd_count].events = POLLIN;
    (*fds)[*fd_count].revents = 0;
    (*fd_count)++;
 }

 void del_fd(struct pollfd fds[], int *fd_count, int i) {
    fds[i] = fds[*fd_count - 1];
    (*fd_count)--;
 }

 /*
 Making sure to write all bytes frotom a file
 */
int writeall(int fd, void *buf, long *filesize) {
    int total = 0; // how many bytes we've sent
    int bytesleft = *filesize; //how many bytes we have left to send
    int n;

    while (total < *filesize) {
        n = write(fd,buf + total, bytesleft);
        if (n == -1) {
            break;
        }
        total += n;
        bytesleft -=n;
    }
    *filesize = total; // Actual vfilesize value to return.

    return n==-1? 1:0; // returns -1 on failure, 0 on success

}

 /*
Sending the webpage to the client
*/
void send_webpage(int fd, int *fd_count, int i) {
    //char hbuf[] = "Connected to port 9034!\n";
    FILE *chatbox_page = fopen("chatpage.html", "r");
    if (chatbox_page == NULL) {
        printf("webserver: chatpage.html failed to open");
        exit(1);
    }

    fseek(chatbox_page, 0L, SEEK_END);
    long filesize = ftell(chatbox_page);
    rewind(chatbox_page);

    void *body = malloc(filesize + 1);
    fread(body,1, filesize,chatbox_page);
    //body[filesize] = '\0';
    fclose(chatbox_page);
    //char headers[1024]; 
    char headers[1024];
    snprintf(headers,sizeof(headers),
        "HTTP/1.0 200 OK\r\n"
        "Server: webserver-c\r\n"
        "Content-type: text/html\r\n"
        "Content-Length: %ld\r\n\r\n",filesize);
    
    
    long headers_size = strlen(headers);
    int writeval_headers = write(fd,headers, headers_size);
    int writeval = writeall(fd,body,&filesize);

    if (writeval_headers < 0 /*|| writeval < 0*/) {
        perror("webserver: send_webpage failed");
        exit(1);
    }
    free(body);
}

/*
 * Handle incoming connections.
 */
 void handle_new_connection(int listener, int *fd_count, int *fd_size, struct pollfd **fds) {
    struct sockaddr_storage remoteaddr; // Client address
    socklen_t addrlen;
    int newfd; // Newly accept()ed socket descriptor
    char remoteIP[INET6_ADDRSTRLEN];
    int buffer_size = 1024;
    char client_buffer[buffer_size];

    addrlen = sizeof remoteaddr;
    newfd = accept(listener, (struct sockaddr *)&remoteaddr, &addrlen); // Might need to change into websocket

    if (newfd == -1) {
        perror("accept");
    }
    else {
        // Read from the socket
        int readval = read(newfd, client_buffer,buffer_size);
        if (readval < 0) {
            perror("webserver: (failed to read)");
        }

        // Read the request
        char method[buffer_size], uri[buffer_size], version[buffer_size];
        inet_ntop2(&remoteaddr, remoteIP, sizeof remoteIP);
        sscanf(client_buffer, "%s %s %s", method, uri, version);
        printf("Client request from %s on socket %d: %s, %s, %s\n", remoteIP, newfd, method, uri, version);
        
        send_webpage(newfd, fd_count, *fd_count);
        add_to_fds(fds, newfd, fd_count, fd_size);
        printf("pollserver: new connection from %s on socket %d\n", remoteIP, newfd);
    }
}


/*
Process all existing connections
*/
void process_connections(int listener, int *fd_count, int *fd_size, struct pollfd **fds) {
    for (int i = 0; i < *fd_count; i++) {

        // Check if someone's ready to read
        if((*fds)[i].revents & (POLLIN | POLLHUP)) {
            // We got one!

            if ((*fds)[i].fd == listener) {
                // If we're the listener, it's a new connection
                handle_new_connection(listener, fd_count, fd_size, fds);
            }
            else {
                (*fd_count)--;
                close((*fds)[i].fd);
                del_fd(*fds,fd_count,i);
            }
            // else {
            //     // Otherwise we're just a regular client
            //     //handle_client_data(listener, fd_count, *fds, &i);
            //     send_webpage(*fds,fd_count,i);
            // }
        }
    }
}
int main(void) {
    int listener;       // Listening socket descriptor

    // Start off with room for five connections
    // (We'll realloc as necessary)
    int fd_size = 5;
    int fd_count = 0;
    struct pollfd *fds = malloc(sizeof *fds * fd_size);

    listener = get_listener_socket();


    if (listener == -1) {
        fprintf(stderr, "error getting listening socket\n");
        exit(-1);
    }

    // Add the listener to set;
    // Report ready to read on incoming connections
    fds[0].fd = listener;
    fds[0].events = POLLIN;

    fd_count++; // For the listener

    puts("pollserver: waiting for connection:...");

    // Main loop
    for(;;) {
        int poll_count = poll(fds, fd_count, -1);

        if (poll_count == -1) {
            perror("poll");
            exit(1);
        }

        // Run through connections looking for data to read
        process_connections(listener, &fd_count, &fd_size, &fds);
    }

    free(fds);
 }
