#pragma once

#include "HTTPStruct.hpp"
#include "SERVERStruct.hpp"
#include "Macro.hpp"

int parse_parameters(std::unordered_map<std::string, std::string>& config, int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string para = argv[i];
        if (para == "-h") {
            ++i;
            if (i >= argc) std::cerr << "should provide hostname after -h" << std::endl;
            config["host"] = argv[i];
        } else if (para == "-p") {
            ++i;
            if (i >= argc) std::cerr << "should provide port after -p" << std::endl;
            config["port"] = argv[i];
        } else {
            std::cerr << "What is this? (" << para << ")" << std::endl;
        }
    }

    if (config.find("host") != config.end() && config.find("port") != config.end()) return 0;

    std::cerr << "usage: [exe] -h [hostname] -p [port]" << std::endl;
    return -1;
}

void show_req(SIMPLE_HTTP_REQ &req_struct) {
    std::cerr << "---------------------------------" << std::endl;
    std::cerr << "HTTP version: " << req_struct.version << std::endl;
    std::cerr << "HTTP req_path: " << req_struct.req_path << std::endl;
    std::cerr << "HTTP method: " << req_struct.method << std::endl;
    std::cerr << "---------------------------------" << std::endl;
}

void show_resp(SIMPLE_HTTP_RESP &resp_struct) {
    std::cerr << "---------------------------------" << std::endl;
    std::cerr << "HTTP data: " << resp_struct.data << std::endl;
    std::cerr << "---------------------------------" << std::endl;
}

int create_server_socket(std::unordered_map<std::string, std::string>& config, int& sfd) {
    // fill the struct addrinfo, and declare 2 pointers to retrieve search results
    // SOCK_STREAM: TCP; DATA_STREAM: UDP
    // AI_PASSIVE: localhost
    addrinfo hints {
        .ai_flags = AI_PASSIVE,
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = 0,
        .ai_addrlen = 0,
        .ai_addr = nullptr,
        .ai_canonname = nullptr,
        .ai_next = nullptr
    }, *results, *rptr;

    // use above hints to search suitable configuration
    int s;
    s = getaddrinfo(nullptr, config["port"].c_str(), &hints, &results);
    if (s != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(s) << std::endl;
        return -1;
    }

    // iterate through all the results
    for (rptr = results; rptr != nullptr; rptr = rptr->ai_next) {
        // create socket
        sfd = socket(rptr->ai_family, rptr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, rptr->ai_protocol);
        if (sfd == -1) continue;

        // skip TIME_WAIT after close()
        int optionval = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &optionval, sizeof(optionval));

        // if create socket success, bind to address
        // break if success
        if (bind(sfd, rptr->ai_addr, rptr->ai_addrlen) == 0) break;

        close(sfd);
    }

    // release search results
    freeaddrinfo(results);

    // if there is no result succeed
    if (rptr == nullptr) {
        std::cerr << "Could not bind." << std::endl;
        return -1;
    }

    // listen
    int req_q_len = 100;
    if (listen(sfd, req_q_len) != 0) {
        std::cerr << "Error occurred during listen(). errno = " << errno << std::endl;
        return -1;
    }

    return 0;
}

int create_client_socket(std::unordered_map<std::string, std::string>& config, int& sfd) {
    // fill the struct addrinfo, and declare 2 pointers to retrieve search results
    // SOCK_STREAM: TCP; DATA_STREAM: UDP
    addrinfo hints {
        .ai_flags = 0,
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = 0,
        .ai_addrlen = 0,
        .ai_addr = nullptr,
        .ai_canonname = nullptr,
        .ai_next = nullptr
    }, *results, *rptr;

    // use above hints to search suitable configuration
    int s;
    s = getaddrinfo(config["host"].c_str(), config["port"].c_str(), &hints, &results);
    if (s != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(s) << std::endl;
        return -1;
    }

    // iterate through all the results
    for (rptr = results; rptr != nullptr; rptr = rptr->ai_next) {
        // create socket
        sfd = socket(rptr->ai_family, rptr->ai_socktype | SOCK_CLOEXEC, rptr->ai_protocol);
        if (sfd == -1) continue;

        // if create socket success, bind to address
        // break if success
        if (connect(sfd, rptr->ai_addr, rptr->ai_addrlen) == 0) break;

        close(sfd);
    }

    // release search results
    freeaddrinfo(results);

    // if there is no result succeed
    if (rptr == nullptr) {
        std::cerr << "Could not connect." << std::endl;
        return -1;
    }

    return 0;
}

int accept_new_client(int sfd, int& cfd, sockaddr* client_addr) {
    socklen_t client_addr_size = sizeof(client_addr);
    cfd = accept(sfd, client_addr, &client_addr_size);
    if (cfd == -1) return -1;
    return 0;
}

int create_epoll_instance(int& epollfd) {
    // EPOLL_CLOEXEC: prevent leak after using exec*()
    if ((epollfd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
        // Detail about epoll error number please refer to manual page
        // https://man7.org/linux/man-pages/man2/epoll_create.2.html
        std::cerr << "Error occurred during epoll_create(). errno = " << errno << std::endl;
        return -1;
    }

    return 0;
}

// https://man7.org/linux/man-pages/man2/epoll_ctl.2.html
// add into epollfd interest list
// EPOLLWAKEUP: make device stay active
// EPOLLET(Edge-Trigger): triggers when change state
// Default(Level-Trigger): triggers when in specified state
// EPOLLEXCLUSIVE: in multithread scenario, prevent multiple epollfd to be trigger by the same event
int add_epoll_interest(const EPOLL_INFO& epoll_info, int fd) {
    epoll_event epevent {
        .events = epoll_info.epoll_event_types,
        .data = epoll_data_t {
            .fd = fd
        }
    };

    if (epoll_ctl(epoll_info.epollfd, EPOLL_CTL_ADD, fd, &epevent) == -1) {
        std::cerr << "Error occurred during epoll_ctl(). errno = " << errno << std::endl;
        return -1;
    }

    return 0;
}

int rm_epoll_interest(const EPOLL_INFO& epoll_info, int fd) {
    if (epoll_ctl(epoll_info.epollfd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        std::cerr << "Error occurred during epoll_ctl(). errno = " << errno << std::endl;
        return -1;
    }

    return 0;
}

//
int wait_for_epoll_events(const EPOLL_INFO& epoll_info, epoll_event* epoll_buffers) {
    int num_of_events;

    if ((num_of_events = epoll_wait(epoll_info.epollfd, epoll_buffers, epoll_info.epoll_buffers_size, epoll_info.epoll_timeout)) == -1) {
        std::cerr << "Error occurred during epoll_wait(). errno = " << errno << std::endl;
        return -1;
    }

    return num_of_events;
}
