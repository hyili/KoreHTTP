#include <iostream>
#include <string>
#include <unordered_map>
#include <regex>
#include <functional>

#include "Utils.hpp"

using namespace std;

class WebClient {
    unordered_map<string, string> config;
    int sfd;
    SIMPLE_HTTP_REQ req_struct;
    SIMPLE_HTTP_RESP resp_struct;
    function<int(int, SIMPLE_HTTP_REQ&)> req_handler;
    function<int(int, SIMPLE_HTTP_RESP&)> resp_handler;

public:
    WebClient() = delete;
    ~WebClient() {disconnect();};
    WebClient(int argc, char** argv) {
        if (parse_parameters(config, argc, argv) == -1) {
            cerr << "Error occurred during parse_parameters()." << endl;
            exit(-1);
        }

        req_handler = [](int sfd, SIMPLE_HTTP_REQ &req_struct) -> int {
            int flags = 0, ret;
            string req;
    
            // data struct serialization
            req += req_struct.method + " " + req_struct.req_path + " " + req_struct.version + "\n";
    
            ret = send(sfd, req.c_str(), req.size(), flags);
            if (ret == -1) {
                cerr << "Error occurred during send(). errno = " << errno << endl;
                return -1;
            }
     
            return 0;
        };
    
        resp_handler = [](int sfd, SIMPLE_HTTP_RESP &resp_struct) -> int {
            string resp;
            int flags = 0, ret;
            char buffer[BUFFER_SIZE];
    
            while (ret = recv(sfd, buffer, BUFFER_SIZE, flags)) {
                if (ret == -1) {
                    cerr << "Error occurred during recv(). errno = " << errno << endl;
                    return -1;
                }
                resp += buffer;
                memset(buffer, 0, BUFFER_SIZE);

                // parse the response
                regex rule("Data in file: (/[^ ]*)\n");
                smatch sm;
                if (!regex_search(resp, sm, rule)) continue;

                // fill into the resp struct
                resp_struct = {
                    .data = sm[0]
                };
                break;
            }

            return 0;
        };
    }

    int connect() {
        // create socket fd
        if (create_client_socket(config, sfd) != 0) {
            cerr << "Error occurred during create_client_socket()." << endl;
            return -1;
        }

        return 0;
    }

    int process() {
        // going to initiate req_struct
        req_struct = {
            .version = "HTTP/1.1",
            .req_path = "/path/to/webpage.html",
            .method = "GET"
        };
    
        if (req_handler(sfd, req_struct) == -1) {
            cerr << " [X] Connection to server closed. ip: " << config["host"] << ", port: " << config["port"] << endl;
            disconnect();
            return -1;
        }
    
        if (resp_handler(sfd, resp_struct) == -1) {
            cerr << " [X] Connection to server closed. ip: " << config["host"] << ", port: " << config["port"] << endl;
            disconnect();
            return -1;
        }
    
        // print the response out
        show_resp(resp_struct);
    
        cerr << " [*] Connection to server closed. ip: " << config["host"] << ", port: " << config["port"] << endl;
        return 0;
    }

    void disconnect() {
        close(sfd);
    }
};
