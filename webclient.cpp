#include <iostream>
#include <vector>
#include "webclient.hpp"

using namespace std;

#define NUM_OF_CONNECT 1

int main (int argc, char** argv) {
    vector<client::WebClient> WCs = vector<client::WebClient>(NUM_OF_CONNECT, client::WebClient(argc, argv));

    for (int i = 0; i < NUM_OF_CONNECT; ++i) {
        if (WCs[i].connect() == -1) {
            return -1;
        }
    }

    for (int i = 0; i < NUM_OF_CONNECT; ++i) {
        if (WCs[i].process() == -1) {
            return -1;
        }
    }

    for (int i = 0; i < NUM_OF_CONNECT; ++i) {
        WCs[i].disconnect();
    }

    return 0;
}
