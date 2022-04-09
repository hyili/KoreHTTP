#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <regex>
#include <functional>
#include <thread>

#include "Utils.hpp"

using namespace std;

class WebServer {
    unordered_map<string, string> config;
    bool server_inited, server_ready, server_terminated;
    int sfd;
    int epoll_process_fd, epoll_master_fd, epoll_worker_fd;
    unordered_map<int, CLIENT_INFO> waiting_clients;
    unordered_map<int, EPOLL_INFO> epoll_info_table;
    unique_ptr<thread[]> workers;
    uint32_t num_of_connection;
    uint32_t max_num_of_connection;
    uint32_t num_of_workers;
    SIMPLE_HTTP_REQ req_struct;
    SIMPLE_HTTP_RESP resp_struct;
    function<int(CLIENT_INFO&, SIMPLE_HTTP_REQ&, int)> req_handler;
    function<int(CLIENT_INFO&, SIMPLE_HTTP_RESP&, int)> resp_handler;

    void process() {
        int num_of_events = 0, ret;
        EPOLL_INFO& epoll_info = epoll_info_table[epoll_process_fd];
        auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);

        // prepare to accept
        if (add_epoll_interest(epoll_info, sfd) == -1) {
            cerr << "Error occurred during add_epoll_interest()." << endl;
        }

        // processing
        while (!server_terminated) {
            // reset the variables
            num_of_events = 0;
            memset(epoll_buffers.get(), 0, sizeof(epoll_buffers.get()));

            // if no event is polled back
            if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                cerr << " [*] Nobody comes in. timeout = " << epoll_info.epoll_timeout << ", num_of_events = " << num_of_events << endl;
                continue;
            }

            // iterate each polled events
            for (int index = 0; index < num_of_events; index++) {
                int currfd = epoll_buffers.get()[index].data.fd;

                // if new client comes in
                if (currfd == sfd) {
                    // accept clients
                    if ((ret = check_for_client(epoll_process_fd)) < 0) {
                        // do nothing
                    }
                    continue;
                }

                if ((ret = check_for_client_request(currfd)) < 0) {
                    // data transmission is not complete
                    if (ret == -2) continue;
                    // error occurred
                    disconnect_client(epoll_process_fd, currfd);
                } else {
                    // a client is allowed to send/recv 1 http req/resp each connection
                    // and it's done
                    disconnect_client(epoll_process_fd, currfd);
                }
            }
        }
    }

    /* TODO: thread safety
     * this function may
     * modify epoll_buffers => but this is only for local thread => it's okay
     * modify waiting_clients => would add client into this map, but the key is fd, and it is auto-increment. is this okay?
     */
    void master_thread() {
        server_ready = true;

        int num_of_events = 0, ret;
        EPOLL_INFO& epoll_info = epoll_info_table[epoll_master_fd];
        auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);

        // prepare to accept
        if (add_epoll_interest(epoll_info, sfd) == -1) {
            cerr << "Error occurred during add_epoll_interest()." << endl;
        }

        // processing
        while (!server_terminated) {
            // reset the variables
            num_of_events = 0;
            memset(epoll_buffers.get(), 0, sizeof(epoll_buffers.get()));

            // if no event is polled back
            if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                cerr << " [*] Nobody comes in. timeout = " << epoll_info.epoll_timeout << ", num_of_events = " << num_of_events << endl;
                continue;
            }

            // iterate each polled events
            for (int index = 0; index < num_of_events; index++) {
                int currfd = epoll_buffers.get()[index].data.fd;

                // if new client comes in
                if (currfd == sfd) {
                    // accept clients
                    // TODO: waiting_clients RACE?? not sure
                    if ((ret = check_for_client(epoll_worker_fd)) < 0) {
                        // do nothing
                    }
                    continue;
                }
            }
        }
    }

    /* TODO: thread safety
     * this function may:
     * modify epoll_buffers => but this is only for local thread => it's okay
     * modify waiting_clients => would remove client from this map, but the key is fd, and it is auto-increment. is this okay?
     */
    void worker_thread() {
        while (!server_ready) sleep(1);

        int num_of_events = 0, ret;
        EPOLL_INFO& epoll_info = epoll_info_table[epoll_worker_fd];
        auto epoll_buffers = make_unique<epoll_event[]>(epoll_info.epoll_buffers_size);

        // processing
        while (!server_terminated) {
            // reset the variables
            num_of_events = 0;
            memset(epoll_buffers.get(), 0, sizeof(epoll_buffers.get()));

            // if no event is polled back
            if ((num_of_events = wait_for_epoll_events(epoll_info, epoll_buffers.get())) == 0) {
                cerr << " [*] Nobody comes in. timeout = " << epoll_info.epoll_timeout << ", num_of_events = " << num_of_events << endl;
                continue;
            }

            // iterate each polled events
            for (int index = 0; index < num_of_events; index++) {
                int currfd = epoll_buffers.get()[index].data.fd;

                if ((ret = check_for_client_request(currfd)) < 0) {
                    // data transmission is not complete
                    if (ret == -2) continue;
                    // error occurred
                    // TODO: waiting_clients RACE?? not sure
                    disconnect_client(epoll_worker_fd, currfd);
                } else {
                    // a client is allowed to send/recv 1 http req/resp each connection
                    // and it's done
                    // TODO: waiting_clients RACE?? not sure
                    disconnect_client(epoll_worker_fd, currfd);
                }
            }
        }
    }

    int setup_client(int cfd, sockaddr_in& client_addr) {
        if (num_of_connection >= max_num_of_connection) {
            return -1;
        }

        num_of_connection += 1;
        waiting_clients[cfd] = {
            .cfd = cfd,
            .buffer = "",
            .client_addr = client_addr
        };

        return 0;
    }

    void disconnect_client(int epollfd, int cfd) {
        // remove from epoll interest list
        rm_epoll_interest(epoll_info_table[epollfd], cfd);
        // remove client info struct
        waiting_clients.erase(cfd);
        // close client socket
        close(cfd);

        num_of_connection -= 1;
    }

    int check_for_client(int epollfd) {
        int cfd;
        sockaddr_in client_addr;

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
            cerr << " [X] Connection closed due to max_num_of_connection = " << max_num_of_connection << ". ip: " << inet_ntoa(client_addr.sin_addr) << ", port: " << client_addr.sin_port << endl;
            return -1;
        }

        if (add_epoll_interest(epoll_info_table[epollfd], cfd) == -1) {
            cerr << "Error occurred during add_epoll_interest(). cfd = " << cfd << endl;
            cerr << " [X] Connection closed. ip: " << inet_ntoa(client_addr.sin_addr) << ", port: " << client_addr.sin_port << endl;
            return -1;
        }

        cerr << " [*] A new client is coming in. ip: " << inet_ntoa(client_addr.sin_addr) << ", port: " << client_addr.sin_port << endl;
        return 0;
    }

    int check_for_client_request(int cfd) {
        int ret = 0;
        int flags = MSG_DONTWAIT;
        CLIENT_INFO& client_info = waiting_clients[cfd];

        if ((ret = req_handler(client_info, req_struct, flags)) < 0) {
            if (ret == -1) {
                cerr << " [X] Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
                return -1;
            }
            return ret;
        }
        
        show_req(req_struct);

        string resp;
        resp += "Data in file: " + req_struct.req_path + "\n";
        SIMPLE_HTTP_RESP resp_struct{.data = resp};
        if (resp_handler(waiting_clients[cfd], resp_struct, flags) < 0) {
            if (ret == -1) {
                cerr << " [X] Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
                return ret;
            }
            return ret;
        }

        cerr << " [*] Connection to client closed. ip: " << inet_ntoa(client_info.client_addr.sin_addr) << ", port: " << client_info.client_addr.sin_port << endl;
        // close socket after the request/response finished
        return 0;
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

        req_handler = [](CLIENT_INFO& client_info, SIMPLE_HTTP_REQ (&req_struct), int flags) -> int {
            char buffer[BUFFER_SIZE] = {};
            int ret;
            regex rule("(GET|POST|PUT|DELETE) (/[^ ]*) (HTTP/[0-9\\.]+)\n");
            smatch sm;

            while (ret = recv(client_info.cfd, buffer, BUFFER_SIZE, flags)) {
                if (ret == -1) {
                    if (errno = EAGAIN || errno == EWOULDBLOCK) {
                        cerr << "No more data to read." << endl;
                        return -2;
                    }
                    cerr << "Error occurred during recv(). errno = " << errno << endl;
                    return -1;
                }
                client_info.buffer += buffer;
                memset(buffer, 0, BUFFER_SIZE);

                // if not a valid message
                // use search not match here to keep find new request coming (ignore the invalid)
                if (!regex_search(client_info.buffer, sm, rule)) continue;

                // split the request, and fill into req_struct
                req_struct = {
                    .version = sm[3],
                    .req_path = sm[2],
                    .method = sm[1]
                };

                // TODO: remove the trash in client_info.buffer

                break;
            }

            return 0;
        };

        resp_handler = [](CLIENT_INFO& client_info, SIMPLE_HTTP_RESP &resp_struct, int flags) -> int {
            int ret;

            ret = send(client_info.cfd, resp_struct.data.c_str(), resp_struct.data.size(), flags);
            if (ret == -1) {
                if (errno = EAGAIN || errno == EWOULDBLOCK) {
                    // TODO: check how NON-BLOCKING send works
                    cerr << "No data sent." << endl;
                    return -2;
                }
                cerr << "Error occurred during send(). errno = " << errno << endl;
                return -1;
            }
 
            return 0;
        };
    }

    int init() {
        if (server_terminated) return -1;
        server_inited = true;

        // create NON-BLOCKING socket fd
        if (create_server_socket(config, sfd) != 0) {
            cerr << "Error occurred during create_server_socket()." << endl;
            return -1;
        }

        // limitation
        num_of_connection = 0;
        max_num_of_connection = 10;

        // thread workers, 0: for single thread server
        num_of_workers = 4;

        if (num_of_workers == 0) {
            // create epoll instance
            if (create_epoll_instance(epoll_process_fd) != 0) {
                cerr << "Error occurred during create_epoll_instance()." << endl;
                return -1;
            }

            epoll_info_table[epoll_process_fd] = {
                .epollfd = epoll_process_fd,
                .epoll_buffers_size = 10,
                .epoll_timeout = 20000,
                .epoll_event_types = EPOLLIN | EPOLLWAKEUP | EPOLLEXCLUSIVE
            };
        } else {
            workers = make_unique<thread[]>(num_of_workers);
            for (int i = 0; i < num_of_workers; ++i) {
                workers[i] = thread(&WebServer::worker_thread, this);
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
                .epoll_buffers_size = 1,
                .epoll_timeout = 20000,
                .epoll_event_types = EPOLLIN | EPOLLWAKEUP | EPOLLEXCLUSIVE
            };

            epoll_info_table[epoll_worker_fd] = {
                .epollfd = epoll_worker_fd,
                .epoll_buffers_size = 10,
                .epoll_timeout = 20000,
                .epoll_event_types = EPOLLIN | EPOLLWAKEUP | EPOLLEXCLUSIVE
            };
        }

        return 0;
    }

    int start() {
        if (!server_inited) return -1;
        cerr << " [*] WebServer start." << endl;

        if (num_of_workers == 0) {
            process();
        } else {
            // TODO: implement signal handler for join
            // TODO: implement signal terminator for each thread => runtime adjust number of thread
            master_thread();
        }

        return 0;
    }

    void stop() {
        if (server_terminated) return;
        server_terminated = true;
        for (int i = 0; i < num_of_workers; i++) {
            workers[i].join();
        }

        if (num_of_workers == 0) {
            for (auto& [cfd, client_info]: waiting_clients) {
                // remove from epoll interest list
                rm_epoll_interest(epoll_info_table[epoll_process_fd], cfd);
                // close client socket
                close(cfd);

                num_of_connection -= 1;
            }
        } else {
            for (auto& [cfd, client_info]: waiting_clients) {
                // remove from epoll interest list
                rm_epoll_interest(epoll_info_table[epoll_worker_fd], cfd);
                // close client socket
                close(cfd);

                num_of_connection -= 1;
            }
        }

        server_ready = false;
        server_inited = false;

        // close epoll fd
        for (auto& [epollfd, epoll_info]: epoll_info_table) {
            close(epollfd);
        }

        // close server socket
        close(sfd);
    }
};
