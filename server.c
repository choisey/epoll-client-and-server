/*
 * Copyright (c) Seungyeob Choi
 *
 * A TCP server that manages client connections and handles all read and write operations
 * in a single thread using epoll.
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h> // struct sockaddr_in
#include <signal.h>     // sigaction()
#include <stdio.h>
#include <stdlib.h>     // exit()
#include <sys/epoll.h>
#include <unistd.h>     // read(), write(), close()

#define BUFLEN 512
#define PORT 8080

// max number of events that can be returned by epoll at a time
#define MAX_EVENTS 20

// The backlog argument defines the maximum length to which the
// queue of pending connections for sockfd may grow. If a
// connection request arrives when the queue is full, the client may
// receive an error with an indication of ECONNREFUSED or, if the
// underlying protocol supports retransmission, the request may be
// ignored so that a later reattempt at connection succeeds.
#define MAX_BACKLOG 3

void signal_handler(int signo)
{
    switch ( signo )
    {
        case SIGINT:
        case SIGUSR1:
        case SIGUSR2:
        case SIGTERM:
        default:
            fprintf(stderr, "signal received: %d\n", signo);
            break;
    }
}

// should be called when the connection is closed by the peer
static int handle_close(int epollfd, int connfd)
{
    if ( -1 == epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, NULL) )
    {
        switch ( errno )
        {
            case EBADF:
            case EEXIST:
            case EINVAL:
            case ENOENT:
            case ENOMEM:
            case ENOSPC:
            case EPERM:
            default:
                fprintf(stderr, "epoll_ctl error (%d)\n", errno);
                exit(1);
        }
    }

    if ( -1 == close(connfd) )
    {
        switch ( errno )
        {
            case EBADF:
            case EINTR:
            case EIO:
            default:
                fprintf(stderr, "socket close error (%d)\n", errno);
                exit(1);
        }
    }

    return 0;
}

int main()
{
    // custom SIG handler
    // for a graceful server shutdown
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);   // interrupted from keyboard, Ctrl-C.
    sigaction(SIGTERM, &sa, NULL);  // process termination
    sigaction(SIGUSR1, &sa, NULL);  // user defined
    sigaction(SIGUSR2, &sa, NULL);  // user defined

    // create a listener socket

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if ( -1 == listenfd )
    {
        switch ( errno )
        {
            case EACCES:
            case EAFNOSUPPORT:
            case EINVAL:
            case ENFILE:
            case ENOBUFS:
            case ENOMEM:
            case EPROTONOSUPPORT:
            default:
                fprintf(stderr, "socket creation error (%d)\n", errno);
                exit(1);
        }
    }

    int reuse = 1;
    if ( -1 == setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) )
    {
        switch ( errno )
        {
            case EBADF:
            case EDOM:
            case EINVAL:
            case EISCONN:
            case ENOPROTOOPT:
            case ENOTSOCK:
            case ENOMEM:
            case ENOBUFS:
            default:
                fprintf(stderr, "socket setsockopt error (%d)\n", errno);
                exit(1);
        }
    }

    // bind

    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if ( -1 == bind(listenfd, (struct sockaddr*) &servaddr, sizeof(servaddr)) )
    {
        switch (errno )
        {
            case EADDRINUSE:
                fprintf(stderr, "The given address is already in use.\n");
                exit(1);

            case EACCES:
            case EBADF:
            case EINVAL:
            case ENOTSOCK:
            default:
                fprintf(stderr, "socket bind error (%d)\n", errno);
                exit(1);
        }
    }

    // listen

    if ( -1 == listen(listenfd, MAX_BACKLOG) )
    {
        switch ( errno )
        {
            case EADDRINUSE:
            case EBADF:
            case ENOTSOCK:
            case EOPNOTSUPP:
            default:
                fprintf(stderr, "socket listen error (%d)\n", errno);
                exit(1);
        }
    }

    // epoll

    int epollfd = epoll_create1(0);
    if ( -1 == epollfd )
    {
        switch ( errno )
        {
            case EINVAL:
            case EMFILE:
            case ENFILE:
            case ENOMEM:
            default:
                fprintf(stderr, "epoll create1 error (%d)\n", errno);
                exit(1);
        }
    }

    // register listener socket

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listenfd;

    if ( -1 == epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev) )
    {
        switch ( errno )
        {
            case EBADF:
            case EEXIST:
            case EINVAL:
            case ENOENT:
            case ENOMEM:
            case ENOSPC:
            case EPERM:
            default:
                fprintf(stderr, "epoll_ctl error (%d)\n", errno);
                exit(1);
        }
    }

    struct epoll_event events[MAX_EVENTS];

    // event loop

    while ( 1 )
    {
        int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if ( -1 == nfds )
        {
            switch ( errno )
            {
                case EINTR:
                    // A signal was caught
                    fprintf(stderr, "shutting down...\n");
                    if ( -1 == close(listenfd) )
                    {
                        switch ( errno )
                        {
                            case EBADF:
                            case EINTR:
                            case EIO:
                            default:
                                fprintf(stderr, "socket close error (%d)", errno);
                                exit(1);
                        }
                    }
                    exit(0);

                case EBADF:
                case EFAULT:
                case EINVAL:
                default:
                    fprintf(stderr, "epoll_wait error (%d)\n", errno);
                    exit(1);
            }
        }

        for ( int i = 0; i < nfds; i++ )
        {
            // This is declared here to pass it from EPOLLIN to EPOLLOUT in this test implementation.
            // In most other cases, it would likely be placed inside EPOLLIN block.
            size_t total_bytes_in = 0;

            if ( events[i].data.fd == listenfd )
            {
                if ( events[i].events & EPOLLIN )
                {
                    socklen_t addr_size = sizeof(struct sockaddr_in);
                    struct sockaddr_in client_addr;
                    int connfd = accept(listenfd, (struct sockaddr*) &client_addr, &addr_size);
                    if ( -1 == connfd )
                    {
                        switch ( errno )
                        {
                            case EWOULDBLOCK:
                            case EBADF:
                            case ECONNABORTED:
                            case EFAULT:
                            case EINTR:
                            case EINVAL:
                            case ENFILE:
                            case ENOBUFS:
                            case ENOMEM:
                            case ENOTSOCK:
                            case EOPNOTSUPP:
                            case EPERM:
                            case EPROTO:
                            default:
                                fprintf(stderr, "socket accept error (%d)\n", errno);
                                exit(1);
                        }
                    }

                    // set non-blocking

                    int flags = fcntl(connfd, F_GETFL, 0);
                    if ( -1 == flags )
                    {
                        switch ( errno )
                        {
                            case EACCES:
                            case EAGAIN:
                            case EBADF:
                            case EINTR:
                            case EINVAL:
                            case EMFILE:
                            case ENOLCK:
                            case EOVERFLOW:
                            default:
                                fprintf(stderr, "select fcntl error (%d)\n", errno);
                                exit(1);
                        }
                    }

                    if ( -1 == fcntl(connfd, F_SETFL, flags | O_NONBLOCK) )
                    {
                        switch ( errno )
                        {
                            case EACCES:
                            case EAGAIN:
                            case EBADF:
                            case EINTR:
                            case EINVAL:
                            case EMFILE:
                            case ENOLCK:
                            case EOVERFLOW:
                            default:
                                fprintf(stderr, "select fcntl error (%d)\n", errno);
                                exit(1);
                        }
                    }

                    // register the new connection to the rpoll

                    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                    ev.data.fd = connfd;
                    if ( -1 == epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &ev) )
                    {
                        switch ( errno )
                        {
                            case EBADF:
                            case EEXIST:
                            case EINVAL:
                            case ENOENT:
                            case ENOMEM:
                            case ENOSPC:
                            case EPERM:
                            default:
                                fprintf(stderr, "epoll_ctl error (%d)\n", errno);
                                exit(1);
                        }
                    }

                }

                continue;
            }

            if ( events[i].events & EPOLLIN )
            {
                // socket has data to read

                char buffer[BUFLEN];
                ssize_t received;

                while ( 0 < ( received = recv(events[i].data.fd, buffer, sizeof(buffer), 0) ) )
                {
                    char *p = buffer;
                    for ( int i = 0; i < received; i++ )
                    {
                        if ( *p < ' ' && *p != '\n' ) *p = '.';
                        p++;
                    }
                    printf("%.*s", (int) received, buffer);
                    fflush(stdout);

                    total_bytes_in += received;
                }

                switch ( received )
                {
                    case -1:
                        switch ( errno )
                        {
                            case EAGAIN:
                                // no data available right now, try again later...
                                break;

                            case ECONNRESET:
                                // connection reset by the peer
                                handle_close(epollfd, events[i].data.fd);
                                break;

                            case EBADF:
                            case ECONNREFUSED:
                            case EFAULT:
                            case EINTR:
                            case EINVAL:
                            case ENOMEM:
                            case ENOTCONN:
                            case ENOTSOCK:
                            default:
                                fprintf(stderr, "socket recv error (%d)\n", errno);
                                exit(1);
                        }
                        break;

                    default:
                        if ( 0 == total_bytes_in )
                        {
                            // The stream socket peer has performed an orderly shutdown.
                            // recv returning 0 is a socket-closed notification.

                            handle_close(epollfd, events[i].data.fd);
                        }
                }
            }

            if ( events[i].events & EPOLLOUT )
            {
                // socket is ready for writing

                if ( 0 != total_bytes_in )
                {
                    static char ack[] = "Ack\n";

                    int sent = send(events[i].data.fd, ack, sizeof(ack), 0);

                    if ( -1 == sent )
                    {
                        switch ( errno )
                        {
                            case EBADF:
                                // bad file descriptor
                                // the connection was already closed while reading
                                break;

                            case ECONNRESET:
                                // connection reset by the peer
                                handle_close(epollfd, events[i].data.fd);
                                break;

                            case EACCES:
                            case EAGAIN:
                            case EALREADY:
                            case EDESTADDRREQ:
                            case EFAULT:
                            case EINTR:
                            case EINVAL:
                            case EISCONN:
                            case EMSGSIZE:
                            case ENOBUFS:
                            case ENOMEM:
                            case ENOTCONN:
                            case ENOTSOCK:
                            case EOPNOTSUPP:
                            case EPIPE:
                            default:
                                fprintf(stderr, "socket send error (%d)", errno);
                                exit(1);
                        }
                    }
                }
            }

            if ( events[i].events & EPOLLERR )
            {
                // error condition
            }
        }
    }
}
