#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <regex>
#include <functional>
#include <unordered_map>

#include "HTTPStruct.hpp"
#include "Utils.hpp"
#include "Macro.hpp"

#include <arpa/inet.h>

using namespace std;

struct CLIENT_INFO {
    int cfd;
    string buffer;
    sockaddr_in client_addr;
};

class WebServer {
    unordered_map<string, string> config;
    bool inited;
    int sfd, epollfd;
    unordered_map<int, CLIENT_INFO> waiting_clients;
    unique_ptr<epoll_event[]> epoll_buffer;
    size_t epoll_buffer_size;
    uint32_t epoll_timeout;
    uint32_t epoll_event_types;
    uint32_t num_of_connection;
    uint32_t max_num_of_connection;
    SIMPLE_HTTP_REQ req_struct;
    SIMPLE_HTTP_RESP resp_struct;
    function<int(CLIENT_INFO&, SIMPLE_HTTP_REQ&, int)> req_handler;
    function<int(CLIENT_INFO&, SIMPLE_HTTP_RESP&, int)> resp_handler;

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

    void disconnect_client(int cfd) {
        // remove from epoll interest list
        rm_epoll_interest(epollfd, cfd);
        // remove client info struct
        waiting_clients.erase(cfd);
        // close client socket
        close(cfd);

        num_of_connection -= 1;
    }

    int check_for_client() {
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

        if (add_epoll_interest(epollfd, cfd, epoll_event_types) == -1) {
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
    WebServer(int argc, char** argv): inited(false) {
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
        inited = true;

        // create NON-BLOCKING socket fd
        if (create_server_socket(config, sfd) != 0) {
            cerr << "Error occurred during create_server_socket()." << endl;
            return -1;
        }

        // create epoll instance
        if (create_epoll_instance(epollfd) != 0) {
            cerr << "Error occurred during create_epoll_instance()." << endl;
            return -1;
        }

        // limitation
        num_of_connection = 0;
        max_num_of_connection = 10;

        // epoll member initialization
        epoll_buffer_size = 10;
        epoll_timeout = 5000;
        epoll_buffer = make_unique<epoll_event[]>(epoll_buffer_size);
        epoll_event_types = EPOLLIN | EPOLLWAKEUP | EPOLLEXCLUSIVE;

        return 0;
    }

    int start() {
        if (!inited) return -1;
        cerr << " [*] WebServer start." << endl;

        int num_of_events = 0, ret;
        epoll_event* epoll_buffers = epoll_buffer.get();

        // prepare to accept
        if (add_epoll_interest(epollfd, sfd, epoll_event_types) == -1) {
            cerr << "Error occurred during add_epoll_interest()." << endl;
            return -1;
        }

        // processing
        while (true) {
            // reset the variables
            num_of_events = 0;
            memset(epoll_buffers, 0, sizeof(epoll_buffers));

            // if no event is polled back
            if ((num_of_events = wait_for_epoll_events(epollfd, epoll_buffers, epoll_buffer_size, epoll_timeout)) == 0) {
                cerr << " [*] Nobody comes in. timeout = " << epoll_timeout << ", num_of_events = " << num_of_events << endl;
                continue;
            }

            // iterate each polled events
            for (int index = 0; index < num_of_events; index++) {
                int currfd = epoll_buffers[index].data.fd;

                // if new client comes in
                if (currfd == sfd) {
                    // accept clients
                    if ((ret = check_for_client()) < 0) {
                        // do nothing
                    }
                    continue;
                }

                // TODO: run in worker thread
                if ((ret = check_for_client_request(currfd)) < 0) {
                    // data transmission is not complete
                    if (ret == -2) continue;
                    // error occurred
                    disconnect_client(currfd);
                } else {
                    // a client is allowed to send/recv 1 http req/resp each connection
                    // and it's done
                    // TODO: timeout mechanism => support delay break
                    disconnect_client(currfd);
                }
            }
        }

        return 0;
    }

    void stop() {
        // close server socket
        close(sfd);
    }
};
