#include "EpollEchoServer.h"

#include "util/util.h"

#include <cstdlib>
#include <cstring>
#include <queue>

// Linux
#include <cerrno>
#include <error.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

// Process exit code on non recoverable errors
constexpr int EXIT_ERROR = 1;
// Error code returned by Linux APIs
constexpr int ERROR = -1;
// Epoll events we are always interested in
// EPOLLRDHUP:
// Stream socket peer closed connection, or shut down writing
// half of connection.  (This flag is especially useful for
// writing simple code to detect peer shutdown when using
// edge-triggered monitoring.)
constexpr unsigned BASE_EVENTS = EPOLLIN | EPOLLET | EPOLLRDHUP;

// Try and make the given file descriptor non-blocking(O_NONBLOCK)
static void set_non_blocking(int fd) {
    // On error, -1 is returned, and errno is set to indicate the error.
    const auto current_flags = fcntl(fd, F_GETFL);
    if (current_flags == ERROR) {
        close(fd);
        error(EXIT_ERROR, current_flags, "fcntl get");
    }

    const auto fcntl_result = fcntl(fd, F_SETFL, (current_flags | O_NONBLOCK));
    if (fcntl_result == ERROR) {
        close(fd);
        error(EXIT_ERROR, current_flags, "fcntl set");
    }
}

static int create_listening_socket(int port) {
    // The protocol specifies a particular protocol to be used with the
    // socket. Normally only a single protocol exists to support a
    // particular socket type within a given protocol family, in which
    // case protocol can be specified as 0.
    const int fd = socket(AF_INET, SOCK_STREAM, /* protocol */ 0);
    if (fd == ERROR) {
        error(EXIT_ERROR, fd, "socket");
    }

    const int enable_so_reuseaddr = 1;
    // Allow bind even if there are open connections to the previous instance of this program.
    // These connections will hold the TCP port in the TIME_WAIT state for some time i.e. slow...
    // SOL_SOCKET: set options at the socket leve
    const int setsockopt_result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable_so_reuseaddr, sizeof(int));
    if (setsockopt_result == ERROR) {
        close(fd);
        error(EXIT_ERROR, setsockopt_result, "setsockopt");
    }

    sockaddr_in addr{};
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    const auto* bind_addr = reinterpret_cast<const sockaddr*>(&addr);

    const int bind_result = bind(fd, bind_addr, sizeof(addr));
    if (bind_result == ERROR) {
        close(fd);
        error(EXIT_ERROR, bind_result, "bind");
    }

    set_non_blocking(fd);

    // The backlog argument defines the maximum length to which the
    // queue of pending connections for sockfd may grow. If a
    // connection request arrives when the queue is full, the client may
    // receive an error with an indication of ECONNREFUSED or, if the
    // underlying protocol supports retransmission, the request may be
    // ignored so that a later reattempt at connection succeeds.
    const int listen_result = listen(fd, SOMAXCONN);
    if (listen_result == ERROR) {
        close(fd);
        error(EXIT_ERROR, listen_result, "listen");
    }

    return fd;
}

struct Context {
    int fd = -1;
    std::queue<EpollEchoServer::IOBuffer> tx_queue;
};

static Context* create_epoll_ctx(int fd) {
    auto* ctx = new Context;
    ctx->fd = fd;
    return ctx;
}

static void close_connection(const epoll_event& event, std::list<EpollEchoServer::IOBuffer>& free_list) {
    if (event.data.ptr != nullptr) {
        auto* ctx = static_cast<Context*>(event.data.ptr);
        const int fd = ctx->fd;

        // Recycle all the outstanding buffers
        while (!ctx->tx_queue.empty()) {
            auto buffer = ctx->tx_queue.front();
            buffer.length = 0;
            buffer.write_offset = 0;
            free_list.push_back(buffer);
            ctx->tx_queue.pop();
        }

        delete ctx;

        close(fd);
        LOG_INFO("Closed: %d\n", fd);
    }
}

enum class DataType : uint8_t { Fd, Context };
static int register_with_epoll(int fd, int epoll_fd, DataType data_type) {
    epoll_event event{};
    if (data_type == DataType::Fd) {
        event.data.fd = fd;
    } else {
        event.data.ptr = create_epoll_ctx(fd);
    }

    event.events = BASE_EVENTS;
    // When successful, epoll_ctl() returns zero.  When an error occurs,
    // epoll_ctl() returns -1 and errno is set to indicate the error
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
}

static int modify_events(int epoll_fd, Context* ctx, unsigned events) {
    epoll_event event{};
    event.data.ptr = ctx;
    event.events = events;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx->fd, &event);
}

static bool op_would_block(int err) {
    static_assert(EAGAIN == EWOULDBLOCK);
    return err == EWOULDBLOCK;
}

EpollEchoServer::EpollEchoServer(int port) {
    listening_socket_ = create_listening_socket(port);
    epoll_fd_ = epoll_create(MAX_NUM_EVENTS);
    if (epoll_fd_ == ERROR) {
        error(EXIT_ERROR, epoll_fd_, "epoll_create");
    }

    // Listen on readable and peer closed in edge-triggered mode, see 'BASE_EVENTS'
    const auto reg_result = register_with_epoll(listening_socket_, epoll_fd_, DataType::Fd);
    if (reg_result == ERROR) {
        error(EXIT_ERROR, reg_result, "register_with_epoll");
    }

    for (int i = 0; i < NUM_IO_BUFFERS; ++i) {
        IOBuffer buffer{};
        void* data = malloc(IO_BUFFER_SIZE);
        if (data == nullptr) {
            error(EXIT_ERROR, 0, "malloc io buffer");
        }
        buffer.data = static_cast<uint8_t*>(data);
        buffer_free_list_.push_back(buffer);
    }
}

EpollEchoServer::~EpollEchoServer() {
    if (epoll_fd_ != ERROR) {
        close(epoll_fd_);
    }

    if (listening_socket_ != ERROR) {
        close(listening_socket_);
    }

    LOG_INFO("Free list size: %zu", buffer_free_list_.size());

#ifdef INLINE_EPOLL_WRITE
    LOG("inline_errors: %d, inline_would_block_errors: %d", inline_errors_, inline_would_block_errors_);
#endif
}

void EpollEchoServer::run_event_loop(bool& loop) {
    epoll_event events[MAX_NUM_EVENTS];
    // NOTE: a timeout of -1 causes epoll_wait() to block indefinitely
    const int timeout_ms = 100;

    while (loop) {
        // Poll for events
        const int num_events = epoll_wait(epoll_fd_, events, MAX_NUM_EVENTS, timeout_ms);

        for (int i = 0; i < num_events; ++i) {
            const epoll_event& event = events[i];

            if (event.data.fd == listening_socket_) {
                // New connections
                accept_new_connections();
                continue;
            }

            // EPOLLHUP: epoll_wait will always wait for this event; it is not
            // necessary to set it in events (flags) when calling epoll_ctl
            if ((event.events & EPOLLRDHUP) || (event.events & EPOLLHUP)) {
                // EPOLLRDHUP: Stream socket peer closed connection
                // EPOLLHUP:   Indicates that the peer closed its end of the channel
                close_connection(event, buffer_free_list_);
                continue;
            }

            if (event.events & EPOLLIN) {
                // We have new data to read on the socket
                do_read(event);
            } else if (event.events & EPOLLOUT) {
                // We can write to the socket
                do_write(event);
            } else {
                error(EXIT_ERROR, 0, "event type not handled: %u", event.events);
            }
        }
    }
}

void EpollEchoServer::accept_new_connections() {
    // Since we are running in edge-triggered mode we need to handle all
    // outstanding events since we won't be notified about them again...
    while (true) {
        sockaddr in_addr{};
        socklen_t in_len = sizeof(in_addr);
        // On error, -1 is returned, errno is set to indicate the error
        const auto client_fd = accept4(listening_socket_, &in_addr, &in_len, O_NONBLOCK);
        if (client_fd == ERROR) {
            const int error = errno;
            if (op_would_block(error)) {
                // No more connections to accept.
                break;
            }
            LOG_ERROR("Accept error: %d\n", error);
            continue;
        }

        // Successfully accepted the new connection.
        LOG_INFO("New connection: %d\n", client_fd);

        const auto register_result = register_with_epoll(client_fd, epoll_fd_, DataType::Context);
        if (register_result == ERROR) {
            // TODO: We leak the context (epoll data) here, so just exit for now...
            error(EXIT_ERROR, register_result, "register_with_epoll");
        }
    }
}

void EpollEchoServer::do_read(const epoll_event& event) {
    auto* ctx = static_cast<Context*>(event.data.ptr);
    const int fd = ctx->fd;

    // INLINE_EPOLL_WRITE - A "best effort" event loop.
    // We just try to echo what we just read and drop the data on EAGAIN/EWOULDBLOCK or other errors
    // and partial writes...
#ifdef INLINE_EPOLL_WRITE
    // Since we are running in edge-triggered mode we need to read
    // all outstanding data on the socket
    uint8_t buffer[IO_BUFFER_SIZE];
    while (true) {
        // On success, the number of bytes read is returned (0 indicates end of file)
        const auto num_bytes_read = read(fd, buffer, IO_BUFFER_SIZE);
        if (num_bytes_read == 0) {
            // EOF
            close_connection(event, buffer_free_list_);
            break;
        }

        if (num_bytes_read == ERROR) {
            const auto error = errno;
            if (op_would_block(error)) {
                // No more data to read
                break;
            } else {
                LOG_ERROR("Read error: %d\n", error);
                continue;
            }
        }

        LOG_INFO("Read %zd bytes on fd: %d\n", num_bytes_read, ctx->fd);

        // Try and echo the data inline, ignore any errors
        const auto num_bytes_written = write(fd, buffer, num_bytes_read);
        if (num_bytes_written == ERROR) {
            const auto error = errno;
            if (op_would_block(error)) {
                inline_would_block_errors_++;
                // Writing would block e.g. the kernel tcp buffers are full.
                // With INLINE_EPOLL_WRITE we just drop the data...
                LOG_ERROR("Write would block: %d\n", error);
                break;
            } else if (error == EPIPE || error == ECONNRESET) {
                // Broken pipe / Connection reset by peer
                close_connection(event, buffer_free_list_);
                return;
            } else {
                inline_errors_++;
                LOG_ERROR("Write error: %d\n", error);
                close_connection(event, buffer_free_list_);
                return;
            }
        }

        if (num_bytes_read < IO_BUFFER_SIZE) {
            // We have read all readable bytes
            break;
        }
    }
#elif
    // Since we are running in edge-triggered mode we need to read
    // all outstanding data on the socket
    while (true) {
        // Just ignore the events if we run out of buffers, for now...
        if (buffer_free_list_.empty()) {
            LOG_ERROR("Read error due to no buffers on fd: %d\n", ctx->fd);
            break;
        }

        IOBuffer io_buffer = buffer_free_list_.front();
        assert(io_buffer.length == 0u);
        assert(io_buffer.write_offset == 0u);

        // On success, the number of bytes read is returned (0 indicates end of file)
        const auto num_bytes_read = read(fd, io_buffer.data, IO_BUFFER_SIZE);
        if (num_bytes_read == 0) {
            // EOF
            close_connection(event, buffer_free_list_);
            break;
        }

        if (num_bytes_read == ERROR) {
            const auto error = errno;
            if (op_would_block(error)) {
                // No more data to read
                break;
            } else {
                LOG_ERROR("Read error: %d\n", error);
                continue;
            }
        }

        LOG_INFO("Read %zd bytes on fd: %d\n", num_bytes_read, ctx->fd);

        // We managed to read some data, remove the buffer from the free list
        buffer_free_list_.pop_front();
        io_buffer.length = num_bytes_read;
        // Queue the write
        ctx->tx_queue.push(io_buffer);

        // Defer writing until the fd is writeable.
        const auto modify_result = modify_events(epoll_fd_, ctx, BASE_EVENTS | EPOLLOUT);
        if (modify_result == ERROR) {
            error(EXIT_ERROR, modify_result, "modify_events - add writeable");
        }

        if (num_bytes_read < IO_BUFFER_SIZE) {
            // We have read all readable bytes
            break;
        }
    }

    // Try and write/echo the data right away
    do_write(event);
#endif
}

void EpollEchoServer::do_write(const epoll_event& event) {
    auto* ctx = static_cast<Context*>(event.data.ptr);
    const int fd = ctx->fd;
    auto& tx_queue = ctx->tx_queue;

    while (!tx_queue.empty()) {
        IOBuffer& buffer_to_write = tx_queue.front();

#ifdef DEBUG
        // Sanity check
        if (buffer_to_write.write_offset >= buffer_to_write.length) {
            error(EXIT_ERROR, 0, "buffer_to_write.write_offset >= buffer_to_write.length");
        }
#endif

        const unsigned remaining_length = buffer_to_write.length - buffer_to_write.write_offset;
        const void* remaining_buffer = buffer_to_write.data + buffer_to_write.write_offset;

        // On success, the number of bytes written is returned.  On error, -1 is returned, and errno
        // is set to indicate the error.
        const auto num_bytes_written = write(fd, remaining_buffer, remaining_length);
        if (num_bytes_written == ERROR) {
            const auto error = errno;
            if (op_would_block(error)) {
                // Writing would block e.g. the kernel tcp buffers are full
                break;
            } else if (error == EPIPE || error == ECONNRESET) {
                // Broken pipe / Connection reset by peer
                close_connection(event, buffer_free_list_);
                return;
            } else {
                LOG_ERROR("Write error: %d\n", error);
                close_connection(event, buffer_free_list_);
                return;
            }
        }

        buffer_to_write.write_offset += num_bytes_written;

        if (buffer_to_write.write_offset == buffer_to_write.length) {
            // We have written the entire buffer

            // Reset the buffer state and return it to the free list
            buffer_to_write.length = 0;
            buffer_to_write.write_offset = 0;
            tx_queue.pop();
            buffer_free_list_.push_back(buffer_to_write);

            // We do not care about the socket being writeable anymore
            const auto modify_result = modify_events(epoll_fd_, ctx, BASE_EVENTS);
            if (modify_result == ERROR) {
                error(EXIT_ERROR, modify_result, "modify_events - remove writeable");
            }
        }
    }
}
