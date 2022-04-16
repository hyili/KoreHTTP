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
#include "Macro.hpp"

namespace server {
    enum HTTP_PROTO {
        HTTPv1_0,
        HTTPv1_1,
        HTTPv2_0
    };

    struct CLIENT_BUFFER {
        std::string buffer;
        std::unique_ptr<generic::SIMPLE_HTTP_REQ> req_struct;
        std::unique_ptr<generic::SIMPLE_HTTP_RESP> resp_struct;
    };

    struct CLIENT_INFO {
        int cfd;
        bool status;
        sockaddr_in client_addr;
        CLIENT_BUFFER client_buffer;
        uint32_t pending_remove;
    };

    template<typename U>
    class PIPE_MSG {
        U _data;

    public:
        PIPE_MSG(): _data(-1) {};
        PIPE_MSG(U data): _data(data) {};

        void serialize(char* buffer) {
            /*int* ptr = &_data;
            char* cptr = reinterpret_cast<char*>(ptr);
            for (int i = 0; i < sizeof(int); i++) {
                buffer[i] = cptr[i];
            }*/
            U* ptr = reinterpret_cast<U*>(buffer);
            *ptr = _data;
        }

        void deserialize(const char* buffer) {
            /*int* ptr = &_data;
            char* cptr = reinterpret_cast<char*>(ptr);
            for (int i = 0; i < sizeof(int); i++) {
                cptr[i] = buffer[begin+i];
            }*/
            const U* ptr = reinterpret_cast<const U*>(buffer);
            _data = *ptr;
        }

        U getData() {
            return _data;
        }

        void setData(U data) {
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
        void push(T& msg) {
            assert(("pipe not inited", is_inited));
            // TODO: check result
            char data[sizeof(int)];
            msg.serialize(data);
            if (write(pipefd[1], data, sizeof(int)) == -1) {
                std::cerr << "failed to inform workers about the new client" << std::endl;
                std::terminate();
            }
        }

        // push new data into pipe
        void push(T&& msg) {
            assert(("pipe not inited", is_inited));
            // TODO: check result
            char data[sizeof(int)];
            msg.serialize(data);
            if (write(pipefd[1], data, sizeof(int)) == -1) {
                std::cerr << "failed to inform workers about the new client" << std::endl;
                std::terminate();
            }
        }
    };

    template<typename T, size_t msgsize, size_t bufsize>
    class PIPE : public PIPE_SEND<T> {
        // ring buffer
        size_t sz;
        std::array<T, msgsize> msg;
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

        bool isFull() {
            return sz == msgsize;
        }

        void getAll() {
            int ret;

            // drain the pipe buffer
            while (ret = read(this->pipefd[0], buffer, bufsize)) {
                // TODO: check result
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // errno is set only when error occurs, so we should reset it back to 0
                    errno = 0;
                    break;
                }
                data_remain.append(buffer, ret);
                memset(buffer, 0, ret);
            }

            int size_of_msg = sizeof(int);
            int end = size_of_msg;
            const char* head = data_remain.c_str();
            while (!isFull() && (end <= data_remain.size())) {
                msg[tail].deserialize(head);
                head += size_of_msg;
                end += size_of_msg;
                tailIncrease();
                ++sz;
            }
            data_remain.erase(0, end-size_of_msg);
        }

        // retrieve from pipe & return constructed obj pointer
        T* get() {
            int target = head;

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
        PIPE<PIPE_MSG<int>, PIPE_MSG_SIZE, PIPE_BUFFER_SIZE> p;
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
