all:
	g++ -std=c++17 -g -o server -lpthread webserver.cpp
	g++ -std=c++17 -o client webclient.cpp
	g++ -std=c++17 -o test run_test.cpp
