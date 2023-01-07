# io-uring-echo-server

Very simple TCP echo servers based on io_uring and epoll.

# Requirements

Only tested on Ubuntu with kernel version 6.0.9-060009-generic.
Built and tested with clang 14 and g++ 11.3.0.

# Build and run

## Build

Install https://github.com/axboe/liburing.

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/clang++
make
```

## Run (the uring or epoll server)

```
./uring_echo_server
./epoll_echo_server
```

# Example usage

```
jthun@jthun:~$ echo "Hello, World!" | nc localhost 6379
Hello, World!
```

# io-uring resources

* https://kernel.dk/io_uring.pdf
* https://unixism.net/loti/what_is_io_uring.html
* https://github.com/axboe/liburing
