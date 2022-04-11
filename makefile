all: test
	rm -f server client
	g++ -std=c++17 -g -o server webserver.cpp -lpthread
	g++ -std=c++17 -o client webclient.cpp

test:
	rm -f run_test
	g++ -std=c++17 -o run_test run_test.cpp
