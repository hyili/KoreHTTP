#pragma once
#include <string>

namespace generic {
    // enum METHOD {GET, POST, PUT, DELETE};
    struct SIMPLE_HTTP_REQ {
        std::string method;
        std::string uripath;
        std::string version;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };
    
    struct SIMPLE_HTTP_RESP {
        std::string version;
        std::string status;
        std::string message;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };
}
