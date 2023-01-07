#pragma once

#include <cstdint>
#include <list>

struct epoll_event;

class EpollEchoServer {
  public:
    // The maximum number of events returned on each poll
    static constexpr int MAX_NUM_EVENTS = 128;
    // The size of each pre-allocated IO buffer
    static constexpr unsigned IO_BUFFER_SIZE = 1024;
    // The number of IO buffers to pre-allocate
    static constexpr uint16_t NUM_IO_BUFFERS = 1024 * 8;

    struct IOBuffer {
        uint8_t* data = nullptr;
        unsigned length = 0;
        unsigned write_offset = 0;
    };

    explicit EpollEchoServer(int port);
    ~EpollEchoServer();

    void run_event_loop(bool& loop);

  private:
    void accept_new_connections();
    void do_read(const epoll_event& event);
    void do_write(const epoll_event& event);

    int listening_socket_ = -1;
    int epoll_fd_ = -1;
    std::list<IOBuffer> buffer_free_list_;
    unsigned inline_errors_ = 0;
    unsigned inline_would_block_errors_ = 0;
};
