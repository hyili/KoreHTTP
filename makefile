all:
	g++ -std=c++17 -o server webserver.cpp
	g++ -std=c++17 -o client webclient.cpp
	g++ -std=c++17 -o test run_test.cpp
