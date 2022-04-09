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

#define BUFFER_SIZE 1024
#define PROCESS_MODE 3
#define MASTER_MODE 1
#define WORKER_MODE 2
