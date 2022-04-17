KoreHTTP
===
Aims on low latency message exchange.

### Feature
- With C++17 STL + EPOLL + SOCKET/IO
- Data Hotpath optimization
- CPU Affinity
- Fewer branches with hash maps
- Fewer dynamic memory allcation
- Multithread
- Event-Driven

### TODO
- prevent high latency req/resp
    - improve PIPE design
    - improve unordered_map, string... STL design (reduce dynamic memory allocation)
        - reduce processing latency when massive concurrent connection occurs by elimintaing the table erase()
- log by each thread
    - according to the run_test, seems like only 1 worker thread is triggered during each round 10 concurrent connections
- if we add a 10000 loop in master dispatcher, it would be much faster to process the incoming connection
    - which indicates we don't need to go out to the while loop and blocked at epoll_wait(), we can wait inside instead (shorter path)
    - if the connection speed is lower, the latency is higher & throughput is lower
    - if the connection speed is higher, the latency is lower & throughput is higher
    - so, why don't we just use blocked accept on master_thread?
- try to reduce EPOLLONESHOT branch by constexpr if optimization
- timeout disconnection support
- persistent connection support
- eBPF support
- Redis support
- UDP support
- WebSocket support
- HTTP protocol parser

### BUGS
- recycling queue delay issue + EPOLLET duplicate event => COMBO! => replace EPOLLET with EPOLLONESHOT can resolve this => but this needs more syscall => Resolved
    - delay may comes from the latency of sequentially event processing
    - if fd=10 EPOLLET generates 2 EPOLLRDHUP events, 1 is handled by worker and removed by master, then another 1 is handled by worker(epoll_ctl errno = 2), then a new client comes in which is assigned fd=10. but at this time, the remove request of fd=10 comes, master removed the wrong client which has fd=10...
    - may encounter errno = 2 (no such file (client is closed, server not closed)) and errno = 9 (bad file descriptor (client & server are both closed))
        - because fd with EPOLLET may be trigger more than once, during the process of handling previous event
- nc not work => Resolved
- HTTP/1.0 should actively close connection => too slow for master to close() => Resolved
- message out-of-order issue, if the event is handled by different thread => Resolved
- multithreaded server end still have some issue => Resolved
- single thread server end still have some issue => Resolved
- data structore can still be optimized - make use of cache
- return message can still be optimized - reduce branch
- new client message pipe can still be optimized - batch signal with boost::lockfree::queue
- Because the new branchless implementation doesn't drain the request queue, EPOLLET won't be acceptable. using Level Trigger instead => Resolved
- when reading from recv(), and appending string. we should notice the return value of recv() which is the length of result => Resolved
- server terminate issue, memset the epoll_buffers solves the issue, and unexpectedly improves the throughput by 1.5x => Resolved
- something weird happened, see below
    - which means a new file descriptor fd=54,55 are created before we unregister the previous fd=54,55. it always happens in master-worker mode with similar timing(20000\~60000/200000) and amount(64\~65/200000) => PIPE_MSG_SIZE id full
```
\[add_epoll_interest\] Error occurred during epoll_ctl(). errno = 17, fd = 54
\[add_epoll_interest\] Error occurred during epoll_ctl(). errno = 17, fd = 54
\[add_epoll_interest\] Error occurred during epoll_ctl(). errno = 17, fd = 54
\[add_epoll_interest\] Error occurred during epoll_ctl(). errno = 17, fd = 55
\[add_epoll_interest\] Error occurred during epoll_ctl(). errno = 17, fd = 55
\[add_epoll_interest\] Error occurred during epoll_ctl(). errno = 17, fd = 55
```
- PIPE stoi() would failed => Resolved
```
terminate called after throwing an instance of 'std::invalid_argument'
what():  stoi
```

### Done
- refine process_thread, master_thread, and worker_thread with hotpath knowledge, achieve 1.2x throughput
- using C++ standard uniform_int_distribution to generate the random value for request distribution, and improves 1.1x~1.2x througthput
- move regex rule to static to prevent rebuilt, improves 6x throughput
- hash map handler entry to reduce branch
    - Try a EPOLLET friendly case which can reduce the use of request queue & number of system call
    - Try to find the correspond handler by incoming request type
    - always use epoll buffer max size = 1, and O(1) choose strategy to lower the latency
- set CPU affinity
- add compiler optimization option -O3
- signal stop() with SIGINT
    - Epoll Edge-Trigger would eat up all the events, stop() event won't accessible to all workers.
    - Maybe wait for epoll_timeout?
    - Use epoll_pwait() with sigmask to set the wake up signal.
    - epoll_wait() & epoll_pwait() issue
- Thread support
    - waiting_clients may have RACE issue
        - use individual waiting_clients map
            - using thread-safe queue to solve master-worker (write/remove) RACE issue
                - but still have DELETE/READ RACE issue between worker threads
            - using individual waiting_clients to solve worker-worker (read/remove/write) RACE issue
                - but may encounter issue of imbalanced connection distribution
            - dynamically redistribute the connection to prevent the throuput issue
            - ***this is cache friendly, but need individual epoll interest list, and 2 queues(1 for master->worker inform, 1 for worker->master recycling)***
            - Suitable for consistent HTTP connection
        - with different perspective (DEPRECATED!)
            - when new client coming in, master thread do waiting_clients map insert won't affect the reading operation of worker thread
            - when handling coming message, only one worker thread will be activate by epoll events, because of EPOLLEXCLUSIVE flag, and it won't change the map itself
            - when disconnecting client, after worker thread remove fd from epoll interest list, push the fd into the thread-safe queue, and wait for recycling by master thread
            - when recycling, master thread fetch the fd, and erase the entry from waiting_clients
            - ***this is not cache friendly, but need only one waiting_clients map, and 1 queue(for worker->master recycling)***
            - ***Edge-Trigger & Level-Trigger both may wake up the worker multiple times in a specific period of time, which would cause error on waiting_clients RACE condition***
            - ***Use EpollOneShot Instead!***
            - Suitable for non-consistent HTTP connection
    - epoll may have RACE issue => no such issue because of internal lock implementation
        - master-worker (write/remove) RACE issue
            - epoll's implementation includes a mutex lock between EPOLL_CTL_ADD, EPOLL_CTL_DEL, EPOLL_CTL_MOD
            - https://code.woboq.org/linux/linux/fs/eventpoll.c.html
            - https://code.woboq.org/linux/linux/fs/eventpoll.c.html#2108 SYSCALL_DEFINE4
            - but may encounter the performance issue due to locks
        - using individual epoll instances to solve lock issue
            - but may encounter issue of imbalanced connection distribution
        - dynamically redistribute the connection to prevent the throuput issue
    - boost library lockfree queue implementation
        - https://valelab4.ucsf.edu/svn/3rdpartypublic/boost-versions/boost_1_55_0/doc/html/lockfree/examples.html
    - moduo library implementation
        - https://cloud.tencent.com/developer/article/1879424
    - epoll
        - https://treenet.cc/tech/11264.html
        - https://events19.linuxfoundation.org/wp-content/uploads/2018/07/dbueso-oss-japan19.pdf
    - epoll discussion
        - https://groups.google.com/g/linux.kernel/c/yVkl-7IRKwM
    - req_struct & resp_struct also has RACE issue => solve by an new struct CLIENT_BUFFER and aggregate into individual CLIENT_INFO

# Other issue
- perf issue https://bugzilla.redhat.com/show_bug.cgi?id=1448402

# Useful tools
- https://tigercosmos.xyz/post/2020/08/system/perf-basic/
- valgrind --leak-check=full --show-leak-kinds=all ./cmd
- perf record -g -F 10000 ./cmd
