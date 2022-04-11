#include <iostream>
#include <signal.h>
#include "webserver.hpp"

using namespace std;

server::WebServer *ptr = nullptr;

void signal_handler(int sig) {
    if (sig == SIGINT) {
        if (ptr) ptr->stop();
    }

    return;
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);

    server::WebServer WS = server::WebServer(argc, argv);
    ptr = &WS;
    cout << "Initializing..." << endl;
    if (WS.init() == -1) return -1;
    cout << "Start!" << endl;
    if (WS.start() == -1) return -1;
    cout << "Stop!" << endl;
    WS.stop();

    return 0;
}
