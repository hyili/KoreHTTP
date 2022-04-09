#include <iostream>
#include "webserver.hpp"

using namespace std;

int main(int argc, char** argv) {
    WebServer WS = WebServer(argc, argv);
    if (WS.init() == -1) return -1;
    if (WS.start() == -1) return -1;
    WS.stop();

    return 0;
}
