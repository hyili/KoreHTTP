#pragma once
#include <string>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <netdb.h>

struct CLIENT_INFO {
    int cfd;
    std::string buffer;
    sockaddr_in client_addr;
};

struct EPOLL_INFO {
    int epollfd;
    size_t epoll_buffers_size;
    uint32_t epoll_timeout;
    uint32_t epoll_event_types;
    std::unique_ptr<epoll_event[]> epoll_buffers;
};
