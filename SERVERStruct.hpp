#pragma once
#include <string>
#include <initializer_list>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <netdb.h>

#include "HTTPStruct.hpp"

namespace server {
    struct CLIENT_BUFFER {
        std::string buffer;
        generic::SIMPLE_HTTP_REQ req_struct;
        generic::SIMPLE_HTTP_RESP resp_struct;
    };

    struct CLIENT_INFO {
        int cfd;
        sockaddr_in client_addr;
        CLIENT_BUFFER client_buffer;
    };

    struct EPOLL_INFO {
        int epollfd;
        size_t epoll_buffers_size;
        uint32_t epoll_timeout;
        uint32_t epoll_event_types;
    };
}
