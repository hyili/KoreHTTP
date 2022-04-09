### TODO
- Thread support
    - waiting_clients may have RACE issue
        - using thread-safe queue to solve master-worker (write/remove) RACE issue
            - but still have DELETE/READ RACE issue between worker threads
        - using individual waiting_clients to solve worker-worker (read/remove/write) RACE issue
            - but may encounter issue of imbalanced connection distribution
        - dynamically redistribute the connection to prevent the throuput issue
    - epoll may have RACE issue
        - master-worker (write/remove) RACE issue => no such issue
            - epoll's implementation includes a mutex lock between EPOLL_CTL_ADD, EPOLL_CTL_DEL, EPOLL_CTL_MOD
            - https://code.woboq.org/linux/linux/fs/eventpoll.c.html
            - https://code.woboq.org/linux/linux/fs/eventpoll.c.html#2108 SYSCALL_DEFINE4
            - but may encounter the performance issue due to locks
        - using individual epoll instances to solve lock issue
            - but may encounter issue of imbalanced connection distribution
        - dynamically redistribute the connection to prevent the throuput issue
    - moduo library implementation
        - https://cloud.tencent.com/developer/article/1879424
- log by each thread
    - according to the run_test, seems like only 1 worker thread is triggered during each round 10 concurrent connections
- timeout support
