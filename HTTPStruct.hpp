#pragma once
#include <string>

namespace generic {
    // enum METHOD {GET, POST, PUT, DELETE};
    struct SIMPLE_HTTP_REQ {
        std::string version;
        std::string req_path;
        std::string method;
    };
    
    struct SIMPLE_HTTP_RESP {
        std::string data;
    };
}
