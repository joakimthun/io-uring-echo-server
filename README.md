# io-uring-echo-server

Very simple TCP echo servers based on io_uring and epoll. Both servers are single-threaded i.e. they both run a single
event loop. Some very simple benchmark results for both servers can be found below.

# Requirements

Only tested on Ubuntu with kernel version 6.0.9-060009-generic.
Built and tested with clang 14 and g++ 11.3.0.

# io_uring echo server

* ```src/net/UringEchoServer.h```
* Uses multishot variants of accept (```io_uring_prep_multishot_accept```) and recv (```io_uring_prep_recv_multishot```)
* IO buffers are pre-allocated and registered using ```io_uring_register_buf_ring```/```io_uring_buf_ring_init``` and
  recycled once the buffer content has been written. I.e. no more buffer allocations happens after the init.
* All context is kept by serializing what is needed into the 8 bytes provided
  by ```io_uring_sqe::user_data```/```io_uring_cqe::user_data```
* **TODO:** Test ```IORING_SETUP_IOPOLL``` - https://man7.org/linux/man-pages/man2/io_uring_setup.2.html

# epoll echo server

* ```src/net/EpollEchoServer.h```
* The benchmarks below are compiled with ```INLINE_EPOLL_WRITE``` defined i.e. no dynamic buffer management (without ```INLINE_EPOLL_WRITE``` the server is using a per connection tx buffer queue and a global list of pre-allocated IO buffers) and less calls to ```epoll_ctl```.
  For an echo server this kinda works, but we risk dropping repsonses on ```EAGAIN```/```EWOULDBLOCK```, partial scoket writes or other errors
  i.e. not really something that can be done in a real application. **TLDR:** With ```INLINE_EPOLL_WRITE``` we try to
  write/echo what we just read, and ignore any errors.
* Allocates 1 "context" per connection to keep track of FDs and RX buffers (the allocation can technically be removed when
  defining ```INLINE_EPOLL_WRITE``` but since in that mode we only have 1 allocation on every accept/new connection the
  benchmark results does not really change...)

# io_uring vs epoll benchmark results

**NOTE:** These benchmarks are not very scientific. E.g. they are run with the clients and server on the same machine (
Intel i7-12700) i.e. loopback. They do **not** reflect real world performance.

## Set-up

### Client(s)

https://github.com/haraldh/rust_echo_bench

Example command: ```cargo run --release -- --address "localhost:6379" --number 1 --duration 60 --length 1024```

This is the same tool that is used by: https://github.com/frevib/io_uring-echo-server

### Server

Built with the build command below. Servers are started like this:

``` taskset -c 0 ./uring_echo_server```

``` taskset -c 0 ./epoll_echo_server```

Server commit used for the benchmarks: ```b1acab8d13cd2ba58a8caf28618f87e44322a857``` (for config values)

## Results

The numbers in the table are requests/sec calculated by rust_echo_bench. All benchmarks are run with a 1024 byte
payload. And as mentioned above, the benchmarks uses a single event-loop/cpu-core on the server side.

|  Server  | Num connections:  |    1    |   256   |   512   |  1024   |
|:--------:|:-----------------:|:-------:|:-------:|:-------:|:-------:|
| io_uring | 1024 byte payload | 176 402 | 421 246 | 354 244 | 353 988 |
|  epoll   | 1024 byte payload | 169 021 | 421 010 | 405 133 |   :(    |

**NOTE:** With 1024 connections the epoll server gets stuck @ 100% cpu usage, have not looked into why yet...

**TODO:** Run benchmarks in a more realistic set-up.

# Build

Install https://github.com/axboe/liburing.

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/clang++
make
```

# Run (the io_uring or epoll server)

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
