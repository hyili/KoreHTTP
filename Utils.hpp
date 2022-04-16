#pragma once

#include "HTTPStruct.hpp"
#include "SERVERStruct.hpp"
#include "Macro.hpp"

#include <chrono>

#include <pthread.h>

namespace generic {
    void show_req(SIMPLE_HTTP_REQ &req_struct) {
        std::cerr << "---------------------------------" << std::endl;
        std::cerr << "HTTP version: " << req_struct.version << std::endl;
        std::cerr << "HTTP uripath: " << req_struct.uripath << std::endl;
        std::cerr << "HTTP method: " << req_struct.method << std::endl;
        std::cerr << "---------------------------------" << std::endl;
    }
    
    void show_resp(SIMPLE_HTTP_RESP &resp_struct) {
        std::cerr << "---------------------------------" << std::endl;
        std::cerr << "HTTP body: " << resp_struct.body << std::endl;
        std::cerr << "---------------------------------" << std::endl;
    }

    using HRC = std::chrono::high_resolution_clock;
    using HRC_PT = std::chrono::time_point<HRC>;
    using MS = std::chrono::milliseconds;

    static HRC_PT t_start;
    static HRC_PT t_end;

    void clock_start() {
        t_start = HRC::now();
    }

    void clock_end(int ms) {
        t_end = HRC::now();
        auto latency = std::chrono::duration_cast<MS>(t_end-t_start);
        if (latency.count() > MS{ms}.count()) {
            std::cerr << "Latency(ms): " << latency.count() << std::endl;
        }
    }
}
namespace server {
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

    int create_socket(std::unordered_map<std::string, std::string>& config, int& sfd) {
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

    int accept_new_client(int sfd, int& cfd, sockaddr* client_addr) {
        socklen_t client_addr_size = sizeof(client_addr);
        cfd = accept(sfd, client_addr, &client_addr_size);
        if (cfd < 0) return -1;
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
    // EPOLLEXCLUSIVE: in multithread scenario, prevent multiple epollfd to be trigger by the same event => we have only 1 epollfd for multiple worker thread
    int add_epoll_interest(const EPOLL_INFO& epoll_info, int fd, uint32_t repl_event_types) {
        epoll_event epevent {
            .events = repl_event_types ? repl_event_types : epoll_info.epoll_event_types,
            .data = epoll_data_t {
                .fd = fd
            }
        };
    
        if (epoll_ctl(epoll_info.epollfd, EPOLL_CTL_ADD, fd, &epevent) == -1) {
            std::cerr << " [add_epoll_interest] Error occurred during epoll_ctl(). errno = " << errno << ", fd = " << fd << std::endl;
            return -1;
        }
    
        return 0;
    }
    
    int mod_epoll_interest(const EPOLL_INFO& epoll_info, int fd, bool enable) {
        epoll_event epevent {
            .events = epoll_info.epoll_event_types | (enable ? EPOLLOUT : 0),
            .data = epoll_data_t {
                .fd = fd
            }
        };

        if (epoll_ctl(epoll_info.epollfd, EPOLL_CTL_MOD, fd, &epevent) == -1) {
            std::cerr << " [mod_epoll_interest] Error occurred during epoll_ctl(). errno = " << errno << ", fd = " << fd << std::endl;
            return -1;
        }

        return 0;
    }

    int rm_epoll_interest(const EPOLL_INFO& epoll_info, int fd) {
        if (epoll_ctl(epoll_info.epollfd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
            std::cerr << " [rm_epoll_interest] Error occurred during epoll_ctl(). errno = " << errno << ", fd = " << fd << std::endl;
            return -1;
        }
    
        return 0;
    }
    
    //
    int wait_for_epoll_events(const EPOLL_INFO& epoll_info, epoll_event* epoll_buffers) {
        int num_of_events;
    
        if ((num_of_events = epoll_wait(epoll_info.epollfd, epoll_buffers, epoll_info.epoll_buffers_size, epoll_info.epoll_timeout)) == -1) {
            std::cerr << " [wait_for_epoll_events] Error occurred during epoll_wait(). errno = " << errno << std::endl;
            return -1;
        }
    
        return num_of_events;
    }

    void set_cpu_affinity(int cpu_no, THREAD_INFO& thread_info) {
        cpu_set_t cpu_set;
        // reset the set
        CPU_ZERO(&cpu_set);
        // set specific cpu set
        CPU_SET(cpu_no, &cpu_set);

        pthread_setaffinity_np(thread_info.thread_obj.native_handle(), sizeof(cpu_set_t), &cpu_set);
    }

    void set_cpu_affinity(int cpu_no) {
        cpu_set_t cpu_set;
        // reset the set
        CPU_ZERO(&cpu_set);
        // set specific cpu set
        CPU_SET(cpu_no, &cpu_set);

        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
    }
}

namespace client {
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

    int create_socket(std::unordered_map<std::string, std::string>& config, int& sfd) {
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

        //std::cerr << " Connection established using ip: " << inet_ntoa(((sockaddr_in*)rptr->ai_addr)->sin_addr) << ", port: " << ((sockaddr_in*)rptr->ai_addr)->sin_port << std::endl;
    
        return 0;
    }
}
