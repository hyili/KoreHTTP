#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <regex>
#include <functional>
#include <thread>
#include <queue>
#include <random>

#include "Utils.hpp"

using namespace std;

namespace server {
    const static regex rule("(GET|POST|PUT|DELETE) (/[^ ]*) (HTTP/[0-9\\.]+)\r*\n([^\\s]+:( )*[^\\s]+\r*\n)*\r*\n");
    using SIMPLE_HTTP_REQ = generic::SIMPLE_HTTP_REQ;
    using SIMPLE_HTTP_RESP = generic::SIMPLE_HTTP_RESP;

    // TODO: divide into TCPServer & WebServer 2 classes
    class WebServer {
        HTTP_PROTO HTTPVersion;
        bool server_inited, server_ready, server_terminated;
        bool affinity_enabled, use_std_rndgen;
        int sfd, pipefd[2];
        unordered_map<string, string> config;
        unordered_map<int, CLIENT_INFO> global_waiting_clients;
        unordered_map<thread::id, EPOLL_INFO> epoll_info_table;
        unordered_map<thread::id, THREAD_INFO> thread_info_table;
        vector<thread::id> workers;
        EPOLL_INFO process_epoll_info;
        THREAD_INFO master;
        // random number generator
        random_device rnddev;
        mt19937 stdgen;
        uniform_int_distribution<> rndgen;
        uint32_t rrgen;
        uint32_t num_of_connection;
        uint32_t max_num_of_connection;
        uint32_t num_of_workers;
        uint32_t max_num_of_concurrency;
        function<int(const CLIENT_INFO&, CLIENT_BUFFER&, int)> req_handler;
        function<int(const CLIENT_INFO&, CLIENT_BUFFER&, int)> resp_handler;
    
        void process() {
            int num_of_events = 0;
            uint32_t counter = 0;

            unordered_map<int, function<void(epoll_event&)>> handler;
            auto events_filter = EPOLLRDHUP | EPOLLIN | EPOLLOUT;
            auto& epoll_info = process_epoll_info;
            auto epoll_process_fd = epoll_info.epollfd;
            auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);
            constexpr bool EPOLLONESHOT_enabled = PROCESS_EPOLL_MODE & EPOLLONESHOT;

            auto client_disconnect_handler = [&](epoll_event& currevt) -> void {
                // client disconnect request => Okay for EPOLLET, because we close the fd, no more request will come
                auto currfd = currevt.data.fd;

                disconnect_global_client(currfd);
            };

            auto client_connect_handler = [&, EPOLLONESHOT_enabled](epoll_event& currevt) -> void {
                // client connect request => Okay for EPOLLET, because we can drian the incoming client here
                auto currfd = currevt.data.fd;

                while (true) {
                    sockaddr_in client_addr;
                    auto ret = check_for_client(client_addr);
                    if (ret > 1) setup_global_client(ret);
                    if (ret == 1) break;
                }

                if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
            };

            auto client_recv_handler = [&, EPOLLONESHOT_enabled](epoll_event& currevt) -> void {
                // client recv ready request => Okay for EPOLLET, because check_for_client_recv_request will make sure the read is complete or the buffer is empty
                auto currfd = currevt.data.fd;
                auto events = currevt.events;
                auto& client_info = global_waiting_clients[currfd];

                auto ret = check_for_client_recv_request(currfd, events, client_info);
                if (ret == 0) {
                    ret = check_for_client_send_request(currfd, events, client_info);
                    if (ret == 0 && HTTPVersion != HTTPv1_0) {
                        // explicitly re-register for EPOLLONESHOT
                        if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
                        return;
                    }

                    if (ret == 1) {
                        // explicitly re-register for EPOLLONESHOT
                        if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, true);
                        return;
                    }

                    // other error occurred
                    disconnect_global_client(currfd);
                    return;
                }

                if (ret == 1) {
                    // explicitly re-register for EPOLLONESHOT
                    if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
                    return;
                }

                // other error occurred
                disconnect_global_client(currfd);
                return;
            };

            auto client_send_handler = [&, EPOLLONESHOT_enabled](epoll_event& currevt) -> void {
                // client send ready request => Okay for EPOLLET, because check_for_client_send_request will make sure the send is complete or the buffer is full
                auto currfd = currevt.data.fd;
                auto events = currevt.events;
                auto& client_info = global_waiting_clients[currfd];

                auto ret = check_for_client_send_request(currfd, events, client_info);
                if (ret == 0) {
                    // explicitly re-register for EPOLLONESHOT
                    if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
                    return;
                }

                if (ret == 1) {
                    // explicitly re-register for EPOLLONESHOT
                    if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, true);
                    return;
                }

                // other error occurred
                disconnect_global_client(currfd);
                return;
            };

            auto garbage_handler = [&](epoll_event& currevt) -> void {
                cerr << " [*] Process: Weird " << currevt.data.fd << endl;
                return;
            };

            // initialize the handler
            handler[EPOLLRDHUP] = handler[EPOLLRDHUP | EPOLLIN] = handler[EPOLLRDHUP | EPOLLOUT] = handler[EPOLLRDHUP | EPOLLIN | EPOLLOUT] = client_disconnect_handler;
            handler[EPOLLIN] = handler[EPOLLIN | EPOLLOUT] = client_recv_handler;
            handler[EPOLLOUT] = client_send_handler;
            handler[0] = garbage_handler;

            // prepare to accept
            if (add_epoll_interest(epoll_info, sfd, 0) == -1) {
                cerr << "Error occurred during add_epoll_interest()." << endl;
                return;
            }

            // processing
            generic::clock_start();
            while (!server_terminated) {
                if (num_of_events > 120) {
                    cerr << "So many events " << num_of_events << endl;
                    generic::clock_end(50);
                }

                // reset the variables
                memset(epoll_buffers.get(), 0, sizeof(epoll_event)*num_of_events);
                num_of_events = 0;
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [*] Process: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    generic::clock_start();
                    continue;
                }
                generic::clock_start();

                counter += num_of_events;
                for (int i = 0; i < num_of_events; i++) {
                    // classification
                    auto& currevt = epoll_buffers.get()[i];
                    auto currfd = currevt.data.fd;
                    auto events = currevt.events;

                    if (currfd == sfd)
                        client_connect_handler(currevt);
                    else
                        handler[events & events_filter](currevt);
                }

                if (server_terminated) cerr << " [*][" << counter << "] Process: Stop " << endl;
            }
        }
    
        void master_thread() {
            int num_of_events = 0;
            uint32_t counter = 0;
            float rate;

            // block until inited
            while (!server_inited) sleep(1);

            auto thread_id = this_thread::get_id();
            auto& epoll_info = epoll_info_table[thread_id];
            auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);
            constexpr bool EPOLLONESHOT_enabled = MASTER_EPOLL_MODE & EPOLLONESHOT;

            auto client_connect_handler = [&, EPOLLONESHOT_enabled](epoll_event& currevt) -> void {
                // client connect request => Okay for EPOLLET, because we can drian the incoming client here
                auto currfd = currevt.data.fd;
                sockaddr_in client_addr;
                PIPE_MSG<int> temp(0);

                while (true) {
                    // accept clients
                    auto ret = check_for_client(client_addr);
                    if (ret > 1) {
                        // TODO: setup each client entry for master thread, except epoll interest list
                        //setup_client(ret, master, false);

                        // random number generator
                        int random_number = random_number_generator();
                        temp.setData(ret);
                        // TODO: check if pushable
                        thread_info_table[workers[random_number]].p.push(temp);
                    }
                    if (ret == 1) break;
                }

                if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
            };

            auto garbage_handler = [&](epoll_event& currevt) -> void {
                cerr << " [*] Master: Weird " << currevt.data.fd << endl;
                return;
            };

            // block until ready
            while (!server_ready) sleep(1);
    
            // prepare to accept
            if (add_epoll_interest(epoll_info, sfd, 0) == -1) {
                cerr << "Error occurred during add_epoll_interest()." << endl;
                return;
            }
    
            // processing
            while (!server_terminated) {
                // reset the variables
                memset(epoll_buffers.get(), 0, sizeof(epoll_event)*num_of_events);
                num_of_events = 0;
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [*] Master: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    continue;
                }

                counter += num_of_events;
                for (int i = 0; i < num_of_events; i++) {
                    // classification
                    auto& currevt = epoll_buffers.get()[i];
                    auto currfd = currevt.data.fd;
                    auto events = currevt.events;

                    if (currfd == sfd)
                        client_connect_handler(currevt);
                    else
                        garbage_handler(currevt);
                }
    
                if (server_terminated) cerr << " [*][" << counter << "] Master: Stop " << endl;
            }
        }
    
        void worker_thread() {
            int num_of_events = 0;
            uint32_t counter = 0;

            unordered_map<int, function<void(epoll_event&)>> handler;
            auto events_filter = EPOLLRDHUP | EPOLLIN | EPOLLOUT;

            // block until inited
            while (!server_inited) sleep(1);

            auto thread_id = this_thread::get_id();
            auto& epoll_info = epoll_info_table[thread_id];
            auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);
            auto& thread_info = thread_info_table[thread_id];
            auto masterfd = thread_info.p.getExit();
            constexpr bool EPOLLONESHOT_enabled = WORKER_EPOLL_MODE & EPOLLONESHOT;

            // processing
            auto client_disconnect_handler = [&](epoll_event& currevt) -> void {
                // client disconnect request => Okay for EPOLLET, because we close the fd, no more request will come
                auto currfd = currevt.data.fd;

                disconnect_client(currfd, thread_info);
            };

            auto client_connect_handler = [&, EPOLLONESHOT_enabled](epoll_event& currevt) -> void {
                // client connect request => Okay for EPOLLET, because we can drian the incoming client here
                auto currfd = currevt.data.fd;

                // drain the buffer
                thread_info.p.getAll();

                while (true) {
                    // fetch one client message
                    auto ptr = thread_info.p.get();

                    // if no client then continue
                    if (ptr == nullptr) break;

                    // if it is client, read its fd, and run setup
                    auto clientfd = ptr->getData();
                    setup_client(clientfd, thread_info, true);
                    thread_info.p.pop();
                }

                if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
            };

            auto client_recv_handler = [&, EPOLLONESHOT_enabled](epoll_event& currevt) -> void {
                // client recv ready request => Okay for EPOLLET, because check_for_client_recv_request will make sure the read is complete or the buffer is empty
                auto currfd = currevt.data.fd;
                auto events = currevt.events;
                auto& client_info = thread_info.waiting_clients[currfd];

                auto ret = check_for_client_recv_request(currfd, events, client_info);
                if (ret == 0) {
                    ret = check_for_client_send_request(currfd, events, client_info);
                    if (ret == 0 && HTTPVersion != HTTPv1_0) {
                        // explicitly re-register for EPOLLONESHOT
                        if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
                        return;
                    }

                    if (ret == 1) {
                        // explicitly re-register for EPOLLONESHOT
                        if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, true);
                        return;
                    }

                    // other error occurred
                    disconnect_client(currfd, thread_info);
                    return;
                }

                if (ret == 1) {
                    // explicitly re-register for EPOLLONESHOT
                    if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
                    return;
                }

                // other error occurred
                disconnect_client(currfd, thread_info);
                return;
            };

            auto client_send_handler = [&, EPOLLONESHOT_enabled](epoll_event& currevt) -> void {
                // client send ready request => Okay for EPOLLET, because check_for_client_send_request will make sure the send is complete or the buffer is full
                auto currfd = currevt.data.fd;
                auto events = currevt.events;
                auto& client_info = thread_info.waiting_clients[currfd];

                auto ret = check_for_client_send_request(currfd, events, client_info);
                if (ret == 0) {
                    // explicitly re-register for EPOLLONESHOT
                    if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, false);
                    return;
                }

                if (ret == 1) {
                    // explicitly re-register for EPOLLONESHOT
                    if constexpr (EPOLLONESHOT_enabled) mod_epoll_interest(epoll_info, currfd, true);
                    return;
                }

                // other error occurred
                disconnect_client(currfd, thread_info);
                return;
            };

            auto garbage_handler = [&](epoll_event& currevt) -> void {
                cerr << " [*] Worker: Weird " << currevt.data.fd << endl;
                return;
            };

            // initialize the handler
            handler[EPOLLRDHUP] = handler[EPOLLRDHUP | EPOLLIN] = handler[EPOLLRDHUP | EPOLLOUT] = handler[EPOLLRDHUP | EPOLLIN | EPOLLOUT] = client_disconnect_handler;
            handler[EPOLLIN] = handler[EPOLLIN | EPOLLOUT] = client_recv_handler;
            handler[EPOLLOUT] = client_send_handler;
            handler[0] = garbage_handler;

            // block until ready
            while (!server_ready) sleep(1);
    
            while (!server_terminated) {
                // reset the variables
                memset(epoll_buffers.get(), 0, sizeof(epoll_event)*num_of_events);
                num_of_events = 0;
    
                // if no event is polled back
                if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                    cerr << " [*] Worker: Nobody comes in. timeout = " << epoll_info.epoll_timeout << endl;
                    continue;
                }

                counter += num_of_events;
                for (int i = 0; i < num_of_events; i++) {
                    // classification
                    auto& currevt = epoll_buffers.get()[i];
                    auto currfd = currevt.data.fd;
                    auto events = currevt.events;

                    if (currfd == masterfd)
                        client_connect_handler(currevt);
                    else
                        handler[events & events_filter](currevt);
                }

                if (server_terminated) {
                    cerr << " [*][" << counter << "] Worker: Stop " << endl;
                }
            }
        }

        int setup_global_client(int cfd) {
            // TODO: client_addr temporary removed
            global_waiting_clients[cfd] = {
                .cfd = cfd,
                .status = true,
                //.client_addr = client_addr,
                .pending_remove = 0
            };

            // add into epoll interest list
            add_epoll_interest(process_epoll_info, cfd, 0);
            //cerr << " [*] new client from " << cfd << endl;

            return 0;
        }

        int setup_client(int cfd, THREAD_INFO& thread_info, bool enable) {
            // TODO: client_addr temporary removed
            thread_info.waiting_clients[cfd] = {
                .cfd = cfd,
                .status = true,
                //.client_addr = client_addr,
                .pending_remove = 0
            };

            // add into epoll interest list
            if (enable) add_epoll_interest(epoll_info_table[thread_info.tid], cfd, 0);
    
            return 0;
        }

        void disconnect_global_client(int cfd) {
            // remove from epoll interest list
            rm_epoll_interest(process_epoll_info, cfd);

            global_waiting_clients[cfd].status = false;
            global_waiting_clients[cfd].client_buffer.req_struct = nullptr;
            global_waiting_clients[cfd].client_buffer.resp_struct = nullptr;

            if (close(cfd) != 0) cerr << "Close failed on " << cfd << endl;
        }

        void disconnect_client(int cfd, THREAD_INFO& thread_info) {
            // remove from epoll interest list
            rm_epoll_interest(epoll_info_table[thread_info.tid], cfd);

            thread_info.waiting_clients[cfd].status = false;
            thread_info.waiting_clients[cfd].client_buffer.req_struct = nullptr;
            thread_info.waiting_clients[cfd].client_buffer.resp_struct = nullptr;

            if (close(cfd) != 0) cerr << "Close failed on " << cfd << endl;
        }

        int check_for_client(sockaddr_in& client_addr) {
            int cfd;
    
            memset(&client_addr, 0, sizeof(client_addr));
            if (accept_new_client(sfd, cfd, reinterpret_cast<sockaddr*>(&client_addr)) != 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    //cerr << "No connection to accept." << endl;
                    errno = 0;
                    return 1;
                }
                cerr << "Error occurred during accept_new_client()." << endl;
                return -1;
            }

            return cfd;
        }
    
        int check_for_client_recv_request(int cfd, uint32_t events, CLIENT_INFO& client_info) {
            int ret = 0;
            int flags = RECV_FLAGS;
    
            //cerr << " [" << thread_id << "] read something from " << cfd << endl;
            if ((ret = req_handler(client_info, client_info.client_buffer, flags)) == -1) {
                cerr << " [" << this_thread::get_id() << "] Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
                return ret;
            }
            //cerr << " [" << thread_id << "] read success" << endl;

            return ret;
        }

        int check_for_client_send_request(int cfd, uint32_t events, CLIENT_INFO& client_info) {
            int ret = 0;
            int flags = SEND_FLAGS;

            //cerr << " [" << thread_id << "] write something to " << cfd << endl;
            if ((ret = resp_handler(client_info, client_info.client_buffer, flags)) == -1) {
                cerr << " [" << this_thread::get_id() << "] Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
                return ret;
            }
            //cerr << " [" << thread_id << "] write success" << endl;

            return ret;
        }

        int random_number_generator() {
            int random_number = 0;
            float rate;

            // TODO: round robin
            if (++rrgen < num_of_workers) return rrgen;
            else return rrgen = 0;

            if (!use_std_rndgen) {
                while (static_cast<int>(rate = static_cast<float>(rand()) / RAND_MAX) == 1);
                random_number = static_cast<int>(rate * num_of_workers);
            } else random_number = rndgen(stdgen);

            return random_number;
        }
    public:
        WebServer() = delete;
        WebServer(const WebServer&) = delete;
        WebServer(WebServer&&) = delete;
        ~WebServer() {stop();}
        WebServer(int argc, char** argv): server_inited(false), server_ready(false), server_terminated(false), affinity_enabled(false), use_std_rndgen(true), stdgen(rnddev()) {
            if (parse_parameters(config, argc, argv) == -1) {
                cerr << "Error occurred during parse_parameters()." << endl;
                terminate();
            }
    
            req_handler = [](const CLIENT_INFO& client_info, CLIENT_BUFFER& client_buffer, int flags) -> int {
                smatch sm;
                char buffer[RECV_BUFFER_SIZE] = {};
                int ret;
    
                // must drain the read buffer here
                while ((ret = recv(client_info.cfd, buffer, RECV_BUFFER_SIZE, flags))) {
                    if (ret < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            //cerr << "No more data to read." << endl;
                            errno = 0;
                            return 1;
                        }
                        cerr << "Error occurred during recv(). errno = " << errno << endl;
                        return -1;
                    }
                    client_buffer.buffer.append(buffer, ret);
                    memset(buffer, 0, ret);
    
                    // if not a valid message
                    // use search not match here to keep find new request coming (ignore the invalid)
                    if (!regex_search(client_buffer.buffer, sm, rule)) continue;

                    // split the request, and fill into req_struct
                    if (client_buffer.req_struct == nullptr) client_buffer.req_struct = make_unique<SIMPLE_HTTP_REQ>();
                    *(client_buffer.req_struct) = {
                        .method = sm[1],
                        .uripath = sm[2],
                        .version = sm[3]
                    };
  
                    // TODO: reset the request buffer
                    client_buffer.buffer.clear();
    
                    // built response according to request
                    if (client_buffer.resp_struct == nullptr) client_buffer.resp_struct = make_unique<SIMPLE_HTTP_RESP>();
                    client_buffer.resp_struct->body = client_buffer.req_struct->version + " 200 OK\r\nConnection: Keep-alive\r\nContent-Length: 13\r\nContent-Type: text/html\r\n\r\nHello World!!";

                    return 0;
                }
    
                return -1;
            };
    
            resp_handler = [](const CLIENT_INFO& client_info, CLIENT_BUFFER& client_buffer, int flags) -> int {
                const string& body = client_info.client_buffer.resp_struct->body;

                // must drain the send buffer here
                // TODO: make a test to try what happened if buffer is not enough to hold the incoming data
                auto ret = send(client_info.cfd, body.c_str(), body.size(), flags);
                if (ret < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        //cerr << "No data sent." << endl;
                        errno = 0;
                        return 1;
                    }
                    cerr << "Error occurred during send(). errno = " << errno << endl;
                    return -1;
                }

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
    
            // NONBLOCKING pipe
            if (pipe2(pipefd, O_NONBLOCK) == -1) {
                cerr << "Error occurred during pipe()." << endl;
                return -1;
            }

            HTTPVersion = HTTPv1_1;

            // limitation
            num_of_connection = 0;
            max_num_of_connection = MAX_NUM_OF_CONNECTION;
    
            // thread workers, 0: for single thread server
            num_of_workers = NUM_OF_WORKERS;
            max_num_of_concurrency = thread::hardware_concurrency();
            affinity_enabled = num_of_workers < max_num_of_concurrency;
            if (num_of_workers >= max_num_of_concurrency*2) {
                cerr << "Too many workers." << endl;
                terminate();
            }

            // random number generator
            use_std_rndgen = true;
            rrgen = 0;
            rndgen = uniform_int_distribution<>(0, num_of_workers-1);

            // TODO: #include <caasert> not work
            // assert(num_of_workers*2+1 < max_num_of_concurrency, "Too many workers.");
    
            // temp variable
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
                    .epoll_buffers_size = PROCESS_EPOLL_BUFFER_SIZE,
                    .epoll_timeout = PROCESS_EPOLL_TIMEOUT,
                    .epoll_event_types = PROCESS_EPOLL_MODE
                };
                if (affinity_enabled) set_cpu_affinity(cpu_no++);

                // add a event fd for signaling the stop event
                add_epoll_interest(process_epoll_info, pipefd[0], 0);
            } else {
                // don't need master.p.init()
                master = {
                    .thread_obj = thread(&WebServer::master_thread, this)
                };
                master.tid = master.thread_obj.get_id();
                if (affinity_enabled) set_cpu_affinity(cpu_no++, master);

                // create epoll instance
                if (create_epoll_instance(epoll_master_fd) != 0) {
                    cerr << "Error occurred during create_epoll_instance()." << endl;
                    return -1;
                }
    
                epoll_info_table[master.tid] = {
                    .epollfd = epoll_master_fd,
                    .epoll_buffers_size = MASTER_EPOLL_BUFFER_SIZE,
                    .epoll_timeout = MASTER_EPOLL_TIMEOUT,
                    .epoll_event_types = MASTER_EPOLL_MODE
                };

                // add a event fd for signaling the stop event
                add_epoll_interest(epoll_info_table[master.tid], pipefd[0], 0);

                for (int i = 0; i < num_of_workers; ++i) {
                    THREAD_INFO temp = {
                        .thread_obj = thread(&WebServer::worker_thread, this)
                    };
                    temp.tid = temp.thread_obj.get_id();
                    thread_info_table[temp.tid] = move(temp);
                    workers.push_back(temp.tid);
                    if (affinity_enabled) set_cpu_affinity(cpu_no++, thread_info_table[temp.tid]);

                    // create epoll instance
                    if (create_epoll_instance(epoll_worker_fd) != 0) {
                        cerr << "Error occurred during create_epoll_instance()." << endl;
                        return -1;
                    }

                    epoll_info_table[temp.tid] = {
                        .epollfd = epoll_worker_fd,
                        .epoll_buffers_size = WORKER_EPOLL_BUFFER_SIZE,
                        .epoll_timeout = WORKER_EPOLL_TIMEOUT,
                        .epoll_event_types = WORKER_EPOLL_MODE
                    };

                    // initialize pipe
                    thread_info_table[temp.tid].p.init();
                    // add a event fd for signaling the stop event
                    add_epoll_interest(epoll_info_table[temp.tid], pipefd[0], 0);
                    // the pipe for incoming clients
                    add_epoll_interest(epoll_info_table[temp.tid], thread_info_table[temp.tid].p.getExit(), 0);
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
                        if (cfd != pipefd[0] && client_info.status) disconnect_global_client(cfd);
                    }
                } else {
                    master.thread_obj.join();
                    for (auto& [thread_id, thread_info]: thread_info_table) {
                        thread_info.thread_obj.join();
                        for (auto& [cfd, client_info]: thread_info.waiting_clients) {
                            if (cfd != pipefd[0] && client_info.status) disconnect_client(cfd, thread_info);
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
