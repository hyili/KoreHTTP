all:
	rm -f server client
	g++ -std=c++17 -o server webserver.cpp -lpthread
	g++ -std=c++17 -o client webclient.cpp -lpthread

optim:
	rm -f server client
	g++ -std=c++17 -O3 -o server webserver.cpp -lpthread
	g++ -std=c++17 -o client webclient.cpp -lpthread

debug:
	rm -f server client
	g++ -std=c++17 -g3 -o server webserver.cpp -lpthread
	g++ -std=c++17 -o client webclient.cpp -lpthread

test:
	rm -f run_test
	g++ -std=c++17 -o run_test run_test.cpp -lpthread

#profile:
#	perf record -g -F 10000 ./server -h localhost -p 3122
#	perf report -i perf.data
#	perf stat -B ./server -h localhost -p 3122
