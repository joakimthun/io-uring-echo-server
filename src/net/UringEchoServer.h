#pragma once

#include <liburing.h>
#include <netinet/in.h>

class UringEchoServer {
  public:
    // Min number of entries to wait for in the event loop
    static constexpr unsigned NUM_WAIT_ENTRIES = 1;
    // The maximum number of entries to retrieve in a single loop iteration
    static constexpr unsigned CQE_BATCH_SIZE = 32;
    // The size of the SQ. By default, the CQ ring will be twice this number
    static constexpr unsigned NUM_SUBMISSION_QUEUE_ENTRIES = 64;
    // The size of each pre-allocated IO buffer. Power-of-2.
    static constexpr unsigned IO_BUFFER_SIZE = 512;
    // The number of IO buffers to pre-allocate
    static constexpr uint16_t NUM_IO_BUFFERS = NUM_SUBMISSION_QUEUE_ENTRIES * 2;

    explicit UringEchoServer(int port);
    ~UringEchoServer();

    void run_event_loop(bool& loop);

  private:
    void handle_accept(io_uring_cqe* cqe);
    void handle_read(io_uring_cqe* cqe, int client_fd);
    void handle_write(io_uring_cqe* cqe, uint16_t buffer_idx);

    io_uring_sqe* get_sqe();
    void add_accept();
    void add_close(int client_fd);
    void add_recv(int client_fd);
    void add_write(int client_fd, const void* data, unsigned length, uint16_t buffer_idx);

    int listening_socket_ = -1;
    io_uring ring_{};
    io_uring_buf_ring* buf_ring_ = nullptr;
    size_t buf_ring_size_ = 0;
    uint8_t* io_buffers_base_addr_ = nullptr;
    bool ring_initialized_ = false;
    // This will be filled with the address of the peer on accept events
    // We currently use io_uring_prep_multishot_accept so this value might get overridden i.e.
    // these fields are not really usable...
    sockaddr_in client_addr_{};
    socklen_t client_addr_len_ = sizeof(client_addr_);
};