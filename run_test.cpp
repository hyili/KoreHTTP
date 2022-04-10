#include <iostream>
#include <vector>
#include <unistd.h>
#include "webclient.hpp"

using namespace std;

#define NUM_OF_CONNECT 10
#define NUM_OF_ROUND 1

int main(int argc, char** argv) {
    vector<client::WebClient> WCs = vector<client::WebClient>(NUM_OF_CONNECT, client::WebClient(argc, argv));

    for (int i = 0; i < NUM_OF_ROUND; i++) {
        cout << "Round No." << i << endl;
        for (int j = 0; j < NUM_OF_CONNECT; ++j) {
            if (WCs[j].connect() == -1) {
                return -1;
            }
            cout << "Connect No." << j << endl;
        }

        sleep(1);
        for (int j = 0; j < NUM_OF_CONNECT; ++j) {
            if (WCs[j].process() == -1) {
                return -1;
            }
            cout << "Process No." << j << endl;
        }

        sleep(1);
        for (int j = 0; j < NUM_OF_CONNECT; ++j) {
            WCs[i].disconnect();
            cout << "Disconnect No." << j << endl;
        }
        cout << endl;
    }

    return 0;
}
