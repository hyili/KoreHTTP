#pragma once
#include <iostream>
#include <unordered_map>
#include <string>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <netdb.h>

#include <arpa/inet.h>

namespace server {
    #define BUFFER_SIZE 4096
    #define PROCESS_MODE 3
    #define MASTER_MODE 1
    #define WORKER_MODE 2
    #define QUEUE_SIZE 1024

    #define MAX_NUM_OF_CONNECTION 256
    #define NUM_OF_WORKERS 3
}

namespace client {
    #define BUFFER_SIZE 4096
}
