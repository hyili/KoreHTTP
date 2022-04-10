#include <iostream>
#include "webserver.hpp"

using namespace std;

int main(int argc, char** argv) {
    server::WebServer WS = server::WebServer(argc, argv);
    cout << "Initializing..." << endl;
    if (WS.init() == -1) return -1;
    cout << "Start!" << endl;
    if (WS.start() == -1) return -1;
    cout << "Stop!" << endl;
    WS.stop();

    return 0;
}
