#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <regex>
#include <functional>
#include <thread>

#include <boost/lockfree/queue.hpp>

#include "Utils.hpp"

using namespace std;

namespace server {
    // TODO: divide into TCPServer & WebServer 2 classes
    class WebServer {
        string HTTPVersion;
        bool server_inited, server_ready, server_terminated;
        int sfd, pipefd[2];
        int epoll_process_fd, epoll_master_fd, epoll_worker_fd;
        unordered_map<string, string> config;
        unordered_map<int, CLIENT_INFO> waiting_clients;
        unordered_map<int, EPOLL_INFO> epoll_info_table;
        unordered_map<thread::id, THREAD_INFO> workers;
        boost::lockfree::queue<int, boost::lockfree::capacity<QUEUE_SIZE>> disconnected_clients;
        uint32_t num_of_connection;
        uint32_t max_num_of_connection;
        uint32_t num_of_workers;
        function<int(const CLIENT_INFO&, CLIENT_BUFFER&, int)> req_handler;
        function<int(const CLIENT_INFO&, CLIENT_BUFFER&, int)> resp_handler;
    
        void process() {
            int num_of_events = 0, ret;
            uint32_t counter = 0;
            EPOLL_INFO& epoll_info = epoll_info_table[epoll_process_fd];
            auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);
    
            // prepare to accept
            if (add_epoll_interest(epoll_info, sfd, 0) == -1) {
                cerr << "Error occurred during add_epoll_interest()." << endl;
                return;
            }
    
            // processing
            while (!server_terminated) {
                // reset the variables
                num_of_events = 0;
                memset(epoll_buffers.get(), 0, sizeof(epoll_buffers.get()));
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [*] Process: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    continue;
                }
    
                // iterate each polled events
                for (int index = 0; index < num_of_events; index++) {
                    counter += 1;
                    int currfd = epoll_buffers.get()[index].data.fd;
                    uint32_t events = epoll_buffers.get()[index].events;
    
                    // time to stop
                    if (currfd == pipefd[0]) {
                        cerr << " [*][" << counter << "] Process: Stop " << currfd << endl;
                        break;
                    }

                    // if new client comes in
                    if (currfd == sfd) {
                        // accept clients
                        if ((ret = check_for_client(epoll_process_fd)) < 0) {
                            // do nothing
                        }
                        continue;
                    }

                    // handle client actively disconnect
                    if (events & EPOLLRDHUP) {
                        disconnect_client(epoll_process_fd, currfd);
                        remove_disconnected_client(currfd);
                        continue;
                    }

                    // handle client send() and recv()
                    if ((ret = check_for_client_request(currfd, events)) < 0) {
                        // handle request struct not complete
                        if (ret == -2) {
                            // explicitly re-register for EPOLLONESHOT
                            if (epoll_info.epoll_event_types & EPOLLONESHOT) mod_epoll_interest(epoll_info, currfd, true);
                            continue;
                        }

                        // other error occurred
                        disconnect_client(epoll_process_fd, currfd);
                        remove_disconnected_client(currfd);
                    } else {
                        if (HTTPVersion == "1.0") {
                            disconnect_client(epoll_process_fd, currfd);
                            remove_disconnected_client(currfd);
                            continue;
                        }

                        // explicitly re-register for EPOLLONESHOT
                        if (epoll_info.epoll_event_types & EPOLLONESHOT) mod_epoll_interest(epoll_info, currfd, true);
                    }
                }
            }
        }
    
        /* TODO: thread safety
         * this function may
         * modify epoll_buffers => but this is only for local thread => it's okay
         * modify waiting_clients => would add client into this map, but the key is fd, and it is auto-increment. is this okay?
         * modify worker epoll's interest list => would add client into the struct. => it's okay because of the internal mutex
         */
        void master_thread() {
            server_ready = true;
    
            thread::id thread_id = this_thread::get_id();
            int num_of_events = 0, ret, dcfd;
            uint32_t counter = 0;
            EPOLL_INFO& epoll_info = epoll_info_table[epoll_master_fd];
            auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);
    
            // prepare to accept
            if (add_epoll_interest(epoll_info, sfd, 0) == -1) {
                cerr << "Error occurred during add_epoll_interest()." << endl;
                return;
            }
    
            // processing
            while (!server_terminated) {
                // reset the variables
                num_of_events = 0;
                memset(epoll_buffers.get(), 0, sizeof(epoll_buffers.get()));
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [*] Master: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    while (disconnected_clients.pop(dcfd)) remove_disconnected_client(dcfd);
                    continue;
                }
    
                // TODO: can still be optimized
                while (disconnected_clients.pop(dcfd)) remove_disconnected_client(dcfd);

                // iterate each polled events
                for (int index = 0; index < num_of_events; index++) {
                    counter += 1;
                    int currfd = epoll_buffers.get()[index].data.fd;
    
                    // time to stop
                    if (currfd == pipefd[0]) {
                        cerr << " [*][" << counter << "] Master: Stop " << currfd << endl;
                        break;
                    }

                    // if new client comes in
                    if (currfd == sfd) {
                        // accept clients
                        if ((ret = check_for_client(epoll_worker_fd)) < 0) {
                            // do nothing
                        }
                        continue;
                    }
                }
            }
        }
    
        /* thread safety
         * this function may:
         * modify epoll_buffers => but this is only for local thread => it's okay
         * modify waiting_clients => would remove client from this map, but the key is fd, and it is auto-increment. is this okay?
         * modify worker epoll's interest list => would remove client from the struct. it's okay because of the internal mutex
         */
        void worker_thread() {
            while (!server_ready) sleep(1);
    
            thread::id thread_id = this_thread::get_id();
            int num_of_events = 0, ret;
            uint32_t counter = 0;
            EPOLL_INFO& epoll_info = epoll_info_table[epoll_worker_fd];
            auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);
    
            // processing
            while (!server_terminated) {
                // reset the variables
                num_of_events = 0;
                memset(epoll_buffers.get(), 0, sizeof(epoll_buffers.get()));
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [" << thread_id << "] Worker: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    continue;
                }
    
                // iterate each polled events
                for (int index = 0; index < num_of_events; index++) {
                    counter += 1;
                    int currfd = epoll_buffers.get()[index].data.fd;
                    uint32_t events = epoll_buffers.get()[index].events;
    
                    // handle client actively disconnect
                    if (events & EPOLLRDHUP) {
                        disconnect_client(epoll_worker_fd, currfd);
                        disconnected_clients.push(currfd);
                        continue;
                    }

                    // handle client send() and recv()
                    if ((ret = check_for_client_request(currfd, events)) < 0) {
                        // handle request struct not complete
                        if (ret == -2) {
                            // TODO: time to stop, use sigmask with epoll_pwait() would be better?
                            if (currfd == pipefd[0]) {
                                cerr << " [" << thread_id << "][" << counter << "] Worker: Stop " << currfd << endl;
                            }

                            // explicitly re-register for EPOLLONESHOT
                            if (epoll_info.epoll_event_types & EPOLLONESHOT) mod_epoll_interest(epoll_info, currfd, true);
                            continue;
                        }

                        // other error occurred
                        disconnect_client(epoll_worker_fd, currfd);
                        disconnected_clients.push(currfd);
                    } else {
                        if (HTTPVersion == "1.0") {
                            disconnect_client(epoll_worker_fd, currfd);
                            disconnected_clients.push(currfd);
                            continue;
                        }

                        // explicitly re-register for EPOLLONESHOT
                        if (epoll_info.epoll_event_types & EPOLLONESHOT) mod_epoll_interest(epoll_info, currfd, true);
                    }
                }
            }
        }
    
        int setup_client(int cfd, sockaddr_in& client_addr) {
            if (num_of_connection >= max_num_of_connection) {
                return -1;
            }

            // if cfd havent been remove yet,
            if (waiting_clients.find(cfd) != waiting_clients.end()) {
                waiting_clients[cfd] = {
                    .cfd = cfd,
                    .client_addr = client_addr,
                    .pending_remove = waiting_clients[cfd].pending_remove+1
                };
            } else {
                num_of_connection += 1;
                waiting_clients[cfd] = {
                    .cfd = cfd,
                    .client_addr = client_addr,
                    .pending_remove = 0
                };
            }
    
            return 0;
        }
    
        void disconnect_client(int epollfd, int cfd) {
            // remove from epoll interest list
            rm_epoll_interest(epoll_info_table[epollfd], cfd);
        }

        void remove_disconnected_client(int cfd) {
            // remove client info struct
            if (waiting_clients.find(cfd) != waiting_clients.end()) {
                cerr << " [*] Master: Remove " << cfd << endl;
                if (waiting_clients[cfd].pending_remove == 0) {
                    waiting_clients.erase(cfd);
                    num_of_connection -= 1;
                    close(cfd);
                } else {
                    //cerr << "num_of_connection vs waiting_clients.size(): " << num_of_connection << ":" << waiting_clients.size() << endl;
                    cerr << "Possible?" << endl;
                    waiting_clients[cfd].pending_remove -= 1;
                }
            }
        }

        int check_for_client(int epollfd) {
            int cfd;
            sockaddr_in client_addr;
            thread::id thread_id = this_thread::get_id();
    
            memset(&client_addr, 0, sizeof(client_addr));
            if (accept_new_client(sfd, cfd, reinterpret_cast<sockaddr*>(&client_addr)) != 0) {
                if (errno = EAGAIN || errno == EWOULDBLOCK) {
                    cerr << "No connection to accept." << endl;
                    return -2;
                }
                cerr << "Error occurred during accept_new_client()." << endl;
                return -1;
            }
    
            if (setup_client(cfd, client_addr) == -1) {
                cerr << "Error occurred during setup_client()." << endl;
                cerr << " [" << thread_id << "] Master: Connection closed due to max_num_of_connection = " << max_num_of_connection << ". ip: " << inet_ntoa(client_addr.sin_addr) << ", port: " << client_addr.sin_port << endl;
                close(cfd);
                return -1;
            }
    
            if (add_epoll_interest(epoll_info_table[epollfd], cfd, 0) == -1) {
                cerr << "Error occurred during add_epoll_interest(). cfd = " << cfd << endl;
                cerr << " [" << thread_id << "] Master: Connection closed. ip: " << inet_ntoa(client_addr.sin_addr) << ", port: " << client_addr.sin_port << endl;
                remove_disconnected_client(cfd);
                return -1;
            }
    
            cerr << " [*] Master: new client from " << cfd << endl;
            return 0;
        }
    
        int check_for_client_request(int cfd, uint32_t events) {
            int ret = 0;
            int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
            CLIENT_INFO& client_info = waiting_clients[cfd];
            thread::id thread_id = this_thread::get_id();
    
            // if we have something to recv
            if (events & EPOLLIN) {
                cerr << " [" << thread_id << "] Worker: read something from " << cfd << endl;
                if ((ret = req_handler(client_info, client_info.client_buffer, flags)) < 0) {
                    if (ret == -1) {
                        cerr << " [" << thread_id << "] Worker: Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
                        return -1;
                    }
                    // do nothing if EAGAIN
                    if (ret == -2) {}
                }
                cerr << " [" << thread_id << "] Worker: read success" << endl;
            }

            // if we have something to send
            if (client_info.client_buffer.req_struct.version.size() > 0) {
                cerr << " [" << thread_id << "] Worker: write something to " << cfd << endl;
                client_info.client_buffer.resp_struct.body = client_info.client_buffer.req_struct.version + " 200 Ok\r\n\r\n";
                if ((ret = resp_handler(client_info, client_info.client_buffer, flags)) < 0) {
                    if (ret == -1) {
                        cerr << " [" << thread_id << "] Worker: Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
                        return ret;
                    }
                    // do nothing if EAGAIN
                    if (ret == -2) {}
                }
                cerr << " [" << thread_id << "] Worker: write success" << endl;
            }
    
            return ret;
        }
    public:
        WebServer() = delete;
        WebServer(const WebServer&) = delete;
        WebServer(WebServer&&) = delete;
        ~WebServer() {stop();}
        WebServer(int argc, char** argv): server_inited(false), server_ready(false), server_terminated(false) {
            if (parse_parameters(config, argc, argv) == -1) {
                cerr << "Error occurred during parse_parameters()." << endl;
                exit(-1);
            }
    
            req_handler = [](const CLIENT_INFO& client_info, CLIENT_BUFFER& client_buffer, int flags) -> int {
                char buffer[BUFFER_SIZE] = {};
                int ret;
                regex rule("(GET|POST|PUT|DELETE) (/[^ ]*) (HTTP/[0-9\\.]+)\r?\n([^\\s]+:( )*[^\\s]+\r?\n)*\r?\n");
                //regex rule("(GET|POST|PUT|DELETE) (/[^ ]*) (HTTP/[0-9\\.]+)\n");
                smatch sm;
    
                while (ret = recv(client_info.cfd, buffer, BUFFER_SIZE, flags)) {
                    if (ret < 0) {
                        if (errno = EAGAIN || errno == EWOULDBLOCK) {
                            cerr << "No more data to read." << endl;
                            return -2;
                        }
                        cerr << "Error occurred during recv(). errno = " << errno << endl;
                        return -1;
                    }
                    client_buffer.buffer += buffer;
                    memset(buffer, 0, BUFFER_SIZE);
    
                    // if not a valid message
                    // use search not match here to keep find new request coming (ignore the invalid)
                    if (!regex_search(client_buffer.buffer, sm, rule)) continue;
    
                    // split the request, and fill into req_struct
                    client_buffer.req_struct = {
                        .method = sm[1],
                        .uripath = sm[2],
                        .version = sm[3]
                    };
    
                    // TODO: reset the request buffer
                    client_buffer.buffer.clear();
    
                    return 0;
                }
    
                return -1;
            };
    
            resp_handler = [](const CLIENT_INFO& client_info, CLIENT_BUFFER& client_buffer, int flags) -> int {
                int ret;
                const string body = client_info.client_buffer.resp_struct.body;

                ret = send(client_info.cfd, body.c_str(), body.size(), flags);
                if (ret < 0) {
                    if (errno = EAGAIN || errno == EWOULDBLOCK) {
                        cerr << "No data sent." << endl;
                        return -2;
                    }
                    cerr << "Error occurred during send(). errno = " << errno << endl;
                    return -1;
                }

                // TODO: reset the request struct
                client_buffer.req_struct.version = "";
     
                return 0;
            };
        }
    
        int init() {
            if (server_terminated) return -1;
    
            // create NON-BLOCKING socket fd
            if (create_socket(config, sfd) != 0) {
                cerr << "Error occurred during create_socket()." << endl;
                return -1;
            }
    
            // pipe
            if (pipe(pipefd) == -1) {
                cerr << "Error occurred during pipe()." << endl;
                return -1;
            }

            HTTPVersion = "1.0";

            // limitation
            num_of_connection = 0;
            max_num_of_connection = 250;
    
            // thread workers, 0: for single thread server
            num_of_workers = 4;
    
            if (num_of_workers == 0) {
                // create epoll instance
                if (create_epoll_instance(epoll_process_fd) != 0) {
                    cerr << "Error occurred during create_epoll_instance()." << endl;
                    return -1;
                }
    
                // TODO: EPOLLET & EPOLLONESHOT
                epoll_info_table[epoll_process_fd] = {
                    .epollfd = epoll_process_fd,
                    .epoll_buffers_size = 5,
                    .epoll_timeout = 20000,
                    .epoll_event_types = EPOLLIN | EPOLLRDHUP | EPOLLWAKEUP
                };

                // add a event fd for signaling the stop event
                add_epoll_interest(epoll_info_table[epoll_process_fd], pipefd[0], 0);
            } else {
                if (!disconnected_clients.is_lock_free()) {
                    cerr << "Cannot use lock-free queue, CAS not supported." << endl;
                }

                for (int i = 0; i < num_of_workers; ++i) {
                    THREAD_INFO temp = {
                        .thread_obj = thread(&WebServer::worker_thread, this)
                    };
                    temp.tid = temp.thread_obj.get_id();
                    workers[temp.tid] = move(temp);
                }
    
                // create epoll instance
                if (create_epoll_instance(epoll_master_fd) != 0) {
                    cerr << "Error occurred during create_epoll_instance()." << endl;
                    return -1;
                }
    
                // create epoll instance
                if (create_epoll_instance(epoll_worker_fd) != 0) {
                    cerr << "Error occurred during create_epoll_instance()." << endl;
                    return -1;
                }
    
                epoll_info_table[epoll_master_fd] = {
                    .epollfd = epoll_master_fd,
                    .epoll_buffers_size = 5,
                    .epoll_timeout = 20000,
                    .epoll_event_types = EPOLLIN | EPOLLWAKEUP
                };
    
                epoll_info_table[epoll_worker_fd] = {
                    .epollfd = epoll_worker_fd,
                    .epoll_buffers_size = 5,
                    .epoll_timeout = 20000,
                    .epoll_event_types = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT | EPOLLWAKEUP
                };

                // add a event fd for signaling the stop event
                add_epoll_interest(epoll_info_table[epoll_master_fd], pipefd[0], EPOLLIN | EPOLLWAKEUP);
                add_epoll_interest(epoll_info_table[epoll_worker_fd], pipefd[0], 0);
            }

            server_inited = true;
            return 0;
        }
    
        int start() {
            if (!server_inited) return -1;
            cerr << " [*] WebServer start." << endl;
    
            if (num_of_workers == 0) {
                process();
            } else {
                master_thread();
            }
    
            return 0;
        }
    
        void stop() noexcept {
            if (server_terminated) return;
            if (!server_inited) return;

            server_terminated = true;

            // send them signal, and wake them up to stop them
            write(pipefd[1], "stop", 4);

            if (num_of_workers == 0) {
                for (auto& [cfd, client_info]: waiting_clients) {
                    disconnect_client(epoll_process_fd, cfd);
                    disconnected_clients.push(cfd);
                }
            } else {
                for (auto& [worker_id, worker]: workers) {
                    worker.thread_obj.join();
                }

                for (auto& [cfd, client_info]: waiting_clients) {
                    disconnect_client(epoll_worker_fd, cfd);
                    disconnected_clients.push(cfd);
                }
            }

            int dcfd;
            while (disconnected_clients.pop(dcfd)) remove_disconnected_client(dcfd);
    
            server_ready = false;
            server_inited = false;
    
            // close epoll fd
            for (auto& [epollfd, epoll_info]: epoll_info_table) {
                close(epollfd);
            }

            // close pipefd
            close(pipefd[0]);
            close(pipefd[1]);

            // close server socket
            close(sfd);
        }
    };
}
