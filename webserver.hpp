#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <regex>
#include <functional>
#include <thread>

#include "Utils.hpp"

using namespace std;

namespace server {
    // TODO: divide into TCPServer & WebServer 2 classes
    class WebServer {
        HTTP_PROTO HTTPVersion;
        bool server_inited, server_ready, server_terminated;
        int sfd, pipefd[2];
        unordered_map<string, string> config;
        unordered_map<int, CLIENT_INFO> global_waiting_clients;
        unordered_map<thread::id, EPOLL_INFO> epoll_info_table;
        unordered_map<thread::id, THREAD_INFO> workers;
        EPOLL_INFO process_epoll_info;
        THREAD_INFO master;
        uint32_t num_of_connection;
        uint32_t max_num_of_connection;
        uint32_t num_of_workers;
        function<int(const CLIENT_INFO&, CLIENT_BUFFER&, int)> req_handler;
        function<int(const CLIENT_INFO&, CLIENT_BUFFER&, int)> resp_handler;
    
        void process() {
            int num_of_events = 0, ret = 0;
            uint32_t counter = 0;
            auto& epoll_info = process_epoll_info;
            auto epoll_process_fd = epoll_info.epollfd;
            auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);
            bool EPOLLONESHOT_enabled = epoll_info.epoll_event_types & EPOLLONESHOT;
    
            // prepare to accept
            if (add_epoll_interest(epoll_info, sfd, 0) == -1) {
                cerr << "Error occurred during add_epoll_interest()." << endl;
                return;
            }
    
            // processing
            while (!server_terminated) {
                // reset the variables
                ret = 0;
                num_of_events = 0;
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [*] Process: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    continue;
                }
    
                // iterate each polled events
                counter += num_of_events;
                for (int index = 0; index < num_of_events; index++) {
                    auto& epoll_event = epoll_buffers.get()[index];
                    auto currfd = epoll_event.data.fd;
                    auto events = epoll_event.events;
    
                    // time to stop
                    if (currfd == pipefd[0]) {
                        cerr << " [*][" << counter << "] Process: Stop " << currfd << endl;
                        continue;
                    }

                    // handle client actively disconnect
                    if (events & EPOLLRDHUP) {
                        rm_epoll_interest(process_epoll_info, currfd);
                        global_waiting_clients.erase(currfd);
                        close(currfd);
                        continue;
                    }

                    // if new client comes in
                    while (currfd == sfd) {
                        // accept clients
                        sockaddr_in client_addr;
                        if ((ret = check_for_client(client_addr)) < 0) {
                            // do nothing
                            break;
                        }

                        // global waiting clients
                        setup_global_client(ret);
                    }

                    // handle client send() and recv()
                    CLIENT_INFO& client_info = global_waiting_clients[currfd];
                    ret = check_for_client_request(currfd, events, client_info);
                    // handle request struct not complete
                    if (ret == -2) {
                        // explicitly re-register for EPOLLONESHOT
                        if (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, true);
                        continue;
                    }

                    // other error occurred
                    if (ret < 0) {
                        rm_epoll_interest(process_epoll_info, currfd);
                        global_waiting_clients.erase(currfd);
                        close(currfd);
                        continue;
                    }

                    if (HTTPVersion == HTTPv1_0) {
                        rm_epoll_interest(process_epoll_info, currfd);
                        global_waiting_clients.erase(currfd);
                        close(currfd);
                        continue;
                    }

                    // explicitly re-register for EPOLLONESHOT
                    if (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
                }
            }
        }
    
        void master_thread() {
            // block until start()
            while (!server_ready) sleep(1);
    
            thread::id thread_id = this_thread::get_id();
            int num_of_events = 0, ret, dcfd;
            uint32_t counter = 0;
            EPOLL_INFO& epoll_info = epoll_info_table[master.tid];
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
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [*] Master: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    continue;
                }

                // iterate each polled events
                counter += num_of_events;
                for (int index = 0; index < num_of_events; index++) {
                    int currfd = epoll_buffers.get()[index].data.fd;
    
                    // time to stop
                    if (currfd == pipefd[0]) {
                        cerr << " [*][" << counter << "] Master: Stop " << currfd << endl;
                        continue;
                    }

                    // if new client comes in
                    while (currfd == sfd) {
                        // accept clients
                        sockaddr_in client_addr;
                        if ((ret = check_for_client(client_addr)) <= 0) {
                            break;
                        }

                        // TODO: setup each client entry for master thread, except epoll interest list
                        setup_client(ret, master, false);

                        // try RR
                        float rate;
                        while (static_cast<int>(rate = static_cast<float>(rand()) / RAND_MAX) == 1);
                        auto ptr = workers.begin();
                        advance(ptr, static_cast<int>(rate * workers.size()));
                        ptr->second.p.push(PIPE_MSG(ret));
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
            // block until start
            while (!server_ready) sleep(1);
    
            thread::id thread_id = this_thread::get_id();
            int num_of_events = 0, ret;
            uint32_t counter = 0;
            auto& epoll_info = epoll_info_table[thread_id];
            auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);
            auto& thread_info = workers[thread_id];
            auto masterfd = thread_info.p.getExit();
            bool EPOLLONESHOT_enabled = epoll_info.epoll_event_types & EPOLLONESHOT;
    
            // processing
            while (!server_terminated) {
                // reset the variables
                num_of_events = 0;
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [" << thread_id << "] Worker: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    continue;
                }

                // iterate each polled events
                counter += num_of_events;
                for (int index = 0; index < num_of_events; index++) {
                    auto& epoll_event = epoll_buffers.get()[index];
                    auto currfd = epoll_event.data.fd;
                    auto events = epoll_event.events;
    
                    // TODO: time to stop, use sigmask with epoll_pwait() would be better?
                    if (currfd == pipefd[0]) {
                        cerr << " [" << thread_id << "][" << counter << "] Worker: Stop " << currfd << endl;
                        continue;
                    }

                    // handle client actively disconnect
                    if (events & EPOLLRDHUP) {
                        disconnect_client(currfd, thread_info);
                        continue;
                    }

                    // the message comes from master
                    if (currfd == masterfd) {
                        while (true) {
                            // fetch one client message
                            auto ptr = thread_info.p.get();

                            // if no client then continue
                            if (ptr == nullptr) break;

                            // if it is client, read its fd, and run setup
                            auto clientfd = ptr->getData();
                            thread_info.p.pop();
                            setup_client(clientfd, thread_info, true);
                        }

                        // reactivate the pipe, if we use ONESHOT
                        continue;
                    }

                    // handle client send() and recv()
                    CLIENT_INFO& client_info = workers[thread_id].waiting_clients[currfd];
                    ret = check_for_client_request(currfd, events, client_info);
                    // handle request struct not complete
                    if (ret == -2) {
                        // explicitly re-register for EPOLLONESHOT
                        if (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, true);
                        continue;
                    }

                    // other error occurred
                    if (ret < 0) {
                        disconnect_client(currfd, thread_info);
                        continue;
                    }

                    if (HTTPVersion == HTTPv1_0) {
                        disconnect_client(currfd, thread_info);
                        continue;
                    }

                    // explicitly re-register for EPOLLONESHOT, EPOLLOUT is excluded
                    if (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
                }
            }
        }

        int setup_global_client(int cfd) {
            // TODO: client_addr temporary removed
            global_waiting_clients[cfd] = {
                .cfd = cfd,
                //.client_addr = client_addr,
                .pending_remove = 0
            };

            // add into epoll interest list
            add_epoll_interest(process_epoll_info, cfd, 0);
            cerr << " [*] new client from " << cfd << endl;

            return 0;
        }

        int setup_client(int cfd, THREAD_INFO& thread_info, bool enable) {
            // TODO: client_addr temporary removed
            thread_info.waiting_clients[cfd] = {
                .cfd = cfd,
                //.client_addr = client_addr,
                .pending_remove = 0
            };

            // add into epoll interest list
            if (enable) add_epoll_interest(epoll_info_table[thread_info.tid], cfd, 0);
            cerr << " [*] " << (enable ? "Worker:" : "Master:") << " new client from " << cfd << endl;
    
            return 0;
        }

        void disconnect_client(int cfd, THREAD_INFO& thread_info) {
            // remove from epoll interest list
            rm_epoll_interest(epoll_info_table[thread_info.tid], cfd);

            thread_info.waiting_clients.erase(cfd);

            close(cfd);
        }

        int check_for_client(sockaddr_in& client_addr) {
            int cfd;
    
            memset(&client_addr, 0, sizeof(client_addr));
            if (accept_new_client(sfd, cfd, reinterpret_cast<sockaddr*>(&client_addr)) != 0) {
                if (errno = EAGAIN || errno == EWOULDBLOCK) {
                    cerr << "No connection to accept." << endl;
                    return -2;
                }
                cerr << "Error occurred during accept_new_client()." << endl;
                return -1;
            }

            return cfd;
        }
    
        int check_for_client_request(int cfd, uint32_t events, CLIENT_INFO& client_info) {
            int ret = 0;
            int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
            thread::id thread_id = this_thread::get_id();
    
            // if we have something to recv
            if (events & EPOLLIN) {
                //cerr << " [" << thread_id << "] Worker: read something from " << cfd << endl;
                if ((ret = req_handler(client_info, client_info.client_buffer, flags)) == -1) {
                    cerr << " [" << thread_id << "] Worker: Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
                    return ret;
                }
                // do nothing if EAGAIN
                //cerr << " [" << thread_id << "] Worker: read success" << endl;
            }

            // if we have something to send
            if (client_info.client_buffer.req_struct.version.size() > 0) {
                //cerr << " [" << thread_id << "] Worker: write something to " << cfd << endl;
                client_info.client_buffer.resp_struct.body = client_info.client_buffer.req_struct.version + " 200 Ok\r\n\r\n";
                if ((ret = resp_handler(client_info, client_info.client_buffer, flags)) == -1) {
                    cerr << " [" << thread_id << "] Worker: Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
                    return ret;
                }
                // do nothing if EAGAIN
                //cerr << " [" << thread_id << "] Worker: write success" << endl;
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
                terminate();
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

            HTTPVersion = HTTPv1_0;

            // limitation
            num_of_connection = 0;
            max_num_of_connection = 250;
    
            // thread workers, 0: for single thread server
            num_of_workers = 0;
    
            // TODO: cannot exceed maximum number of cpu
            int cpu_no = 0;
            int epoll_master_fd, epoll_worker_fd, epoll_process_fd;
            if (num_of_workers == 0) {
                // create epoll instance
                if (create_epoll_instance(epoll_process_fd) != 0) {
                    cerr << "Error occurred during create_epoll_instance()." << endl;
                    return -1;
                }
    
                process_epoll_info = {
                    .epollfd = epoll_process_fd,
                    .epoll_buffers_size = 1,
                    .epoll_timeout = 20000,
                    .epoll_event_types = EPOLLIN | EPOLLRDHUP | EPOLLWAKEUP
                };

                // add a event fd for signaling the stop event
                add_epoll_interest(process_epoll_info, pipefd[0], 0);
            } else {
                // don't need master.p.init()
                master = {
                    .thread_obj = thread(&WebServer::master_thread, this)
                };
                master.tid = master.thread_obj.get_id();
                set_cpu_affinity(cpu_no++, master);

                // create epoll instance
                if (create_epoll_instance(epoll_master_fd) != 0) {
                    cerr << "Error occurred during create_epoll_instance()." << endl;
                    return -1;
                }
    
                epoll_info_table[master.tid] = {
                    .epollfd = epoll_master_fd,
                    .epoll_buffers_size = 1,
                    .epoll_timeout = 20000,
                    .epoll_event_types = EPOLLIN | EPOLLWAKEUP
                };

                // add a event fd for signaling the stop event
                add_epoll_interest(epoll_info_table[master.tid], pipefd[0], 0);

                for (int i = 0; i < num_of_workers; ++i) {
                    THREAD_INFO temp = {
                        .thread_obj = thread(&WebServer::worker_thread, this)
                    };
                    temp.tid = temp.thread_obj.get_id();
                    workers[temp.tid] = move(temp);
                    set_cpu_affinity(cpu_no++, workers[temp.tid]);

                    // create epoll instance
                    if (create_epoll_instance(epoll_worker_fd) != 0) {
                        cerr << "Error occurred during create_epoll_instance()." << endl;
                        return -1;
                    }

                    epoll_info_table[temp.tid] = {
                        .epollfd = epoll_worker_fd,
                        .epoll_buffers_size = 1,
                        .epoll_timeout = 20000,
                        .epoll_event_types = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT | EPOLLWAKEUP
                    };

                    // initialize pipe
                    workers[temp.tid].p.init();
                    // add a event fd for signaling the stop event
                    add_epoll_interest(epoll_info_table[temp.tid], pipefd[0], 0);
                    // the pipe for incoming clients
                    add_epoll_interest(epoll_info_table[temp.tid], workers[temp.tid].p.getExit(), EPOLLIN | EPOLLET | EPOLLWAKEUP);
                }
            }

            server_inited = true;
            return 0;
        }
    
        int start() {
            if (!server_inited) return -1;
            cerr << " [*] WebServer start." << endl;
    
            if (num_of_workers == 0) {
                cerr << " [*] process mode." << endl;
                process();
            } else {
                server_ready = true;
                while (!server_terminated) sleep(1);
            }
    
            return 0;
        }

        // TODO: Still some errors here
        void stop() noexcept {
            if (server_terminated) return;
            if (!server_inited) return;

            server_terminated = true;

            // send them signal, and wake them up to stop them
            if (write(pipefd[1], "stop", 4) == -1) {
                cerr << " [X] Failed to send signal to workers, force exit." << endl;
            } else {
                if (num_of_workers == 0) {
                    for (auto& [cfd, client_info]: global_waiting_clients) {
                        if (cfd != pipefd[0]) rm_epoll_interest(process_epoll_info, cfd);
                    }
                } else {
                    master.thread_obj.join();
                    for (auto& [thread_id, thread_info]: workers) {
                        thread_info.thread_obj.join();
                        for (auto& [cfd, client_info]: thread_info.waiting_clients) {
                            if (cfd != pipefd[0]) disconnect_client(cfd, thread_info);
                        }
                    }
                }
            }

            server_ready = false;
            server_inited = false;
    
            // close epoll fd
            for (auto& [id, epoll_info]: epoll_info_table) {
                close(epoll_info.epollfd);
            }

            // close pipefd
            close(pipefd[0]);
            close(pipefd[1]);

            // close server socket
            close(sfd);
        }
    };
}
