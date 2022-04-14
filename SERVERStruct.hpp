#pragma once
#include <array>
#include <string>
#include <thread>
#include <cassert>
#include <unordered_map>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>

#include "HTTPStruct.hpp"

namespace server {
    enum HTTP_PROTO {
        HTTPv1_0,
        HTTPv1_1,
        HTTPv2_0
    };

    struct CLIENT_BUFFER {
        std::string buffer;
        generic::SIMPLE_HTTP_REQ req_struct;
        generic::SIMPLE_HTTP_RESP resp_struct;
    };

    struct CLIENT_INFO {
        int cfd;
        sockaddr_in client_addr;
        CLIENT_BUFFER client_buffer;
        uint32_t pending_remove;
    };

    class PIPE_MSG {
        int _data;
        std::string deli;

    public:
        PIPE_MSG(): deli(";"), _data(-1) {};
        PIPE_MSG(int data): deli(";"), _data(data) {};

        std::string serialize() {
            std::string ans = std::to_string(_data) + deli;
            return ans;
        }

        bool deserialize(std::string& in) {
            int loc;
            if ((loc = in.find(deli)) != std::string::npos) {
                std::string ss = in.substr(0, loc);
                //std::cerr << "deser" << ss << ":" << ss.size() << std::endl;
                _data = stoi(ss);
                in.erase(0, loc+1);
                return true;
            }
            return false;
        }

        int getData() {
            return _data;
        }

        void setData(int data) {
            _data = data;
        }
    };

    template<typename T>
    class PIPE_SEND {
    protected:
        bool is_inited;
        int pipefd[2];
        int flags;
    public:
        PIPE_SEND() : is_inited(false), flags(0) {};
        ~PIPE_SEND() {
            if (is_inited) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
        }

        void init() {
            flags |= O_NONBLOCK;
            if (pipe2(pipefd, flags) != 0) {
                std::cerr << "failed to initialize the inform pipe from master to worker" << std::endl;
                std::terminate();
            }
            is_inited = true;
            //std::cerr << "Pipe Init" << std::endl;
            //std::cerr << pipefd[0] << ":" << pipefd[1] << std::endl;
        }

        int getEntry() {
            return pipefd[1];
        }

        int getExit() {
            return pipefd[0];
        }

        // push new data into pipe
        void push(T&& msg) {
            assert(("pipe not inited", is_inited));
            // TODO: check result
            std::string data = msg.serialize();
            if (write(pipefd[1], data.c_str(), data.size()) == -1) {
                std::cerr << "failed to inform workers about the new client" << std::endl;
                std::terminate();
            }
        }
    };

    template<typename T, size_t msgsize, size_t bufsize>
    class PIPE : public PIPE_SEND<T> {
        // ring buffer
        size_t sz;
        std::array<T, msgsize+1> msg;
        char buffer[bufsize];
        std::string data_remain;
        int head, tail;

        void tailIncrease() {
            tail = tail==msgsize ? 0 : tail + 1;
        }

        void headIncrease() {
            head = head==msgsize ? 0 : head + 1;
        }
    public:
        // tail in head out
        PIPE() : PIPE_SEND<T>(), sz(0), head(0), tail(0) {
            memset(buffer, 0, bufsize);
        };
        ~PIPE() {}

        size_t size() {
            return sz;
        }

        // retrieve from pipe & return constructed obj pointer
        T* get() {
            int target = head;
            int ret;

            while (ret = read(this->pipefd[0], buffer, bufsize) >= 0) {
                // TODO: check result
                if (ret == EAGAIN || ret == EWOULDBLOCK) break;
                data_remain += buffer;
                memset(buffer, 0, bufsize);
                while (size() <= msgsize && msg[tail].deserialize(data_remain)) {
                    tailIncrease();
                    ++sz;
                }
            }

            if (size() > 0) {
                return &(msg[target]);
            }

            return nullptr;
        }

        void pop() {
            if (size() > 0) {
                headIncrease();
                --sz;
            }
        }
    };

    struct THREAD_INFO {
        PIPE<PIPE_MSG, 1024, 1024> p;
        std::unordered_map<int, CLIENT_INFO> waiting_clients;
        std::thread thread_obj;
        std::thread::id tid;
    };

    struct EPOLL_INFO {
        int epollfd;
        size_t epoll_buffers_size;
        uint32_t epoll_timeout;
        uint32_t epoll_event_types;
    };
}
