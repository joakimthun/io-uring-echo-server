#include "UringEchoServer.h"

#include <cstdlib>
#include <cstring>

// Linux
#include <error.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef LOGGING_ENABLED
#include <cstdio>

#define LOG_INFO(...) fprintf(stdout, __VA_ARGS__)
#define LOG_ERROR(...) fprintf(stderr, __VA_ARGS__)

#else
#define LOG_INFO(...)                                                                                        \
    do {                                                                                                     \
    } while (0)
#define LOG_ERROR(...)                                                                                       \
    do {                                                                                                     \
    } while (0)
#endif

// Process exit code on non recoverable errors
constexpr int EXIT_ERROR = 1;
// Error code returned by Linux APIs
constexpr int ERROR = -1;
// We only have one group for now...
// TODO: Support different groups and buffer sizes
constexpr int BUFFER_GROUP_ID = 1;

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

static void init_io_uring(io_uring* ring, unsigned num_submission_queue_entries) {
    const unsigned flags = 0;
    // On success, io_uring_queue_init(3) returns 0 and 'ring' will point to the shared memory containing the
    // io_uring queues. On failure -errno is returned.
    const int init_result = io_uring_queue_init(num_submission_queue_entries, ring, flags);
    if (init_result != 0) {
        error(EXIT_ERROR, init_result, "io_uring_queue_init");
    }
}

static constexpr unsigned buffer_ring_size() {
    return (UringEchoServer::IO_BUFFER_SIZE + sizeof(io_uring_buf)) * UringEchoServer::NUM_IO_BUFFERS;
}

static constexpr uint8_t* get_buffer_base_addr(void* ring_addr) {
    return (uint8_t*)ring_addr + (sizeof(io_uring_buf) * UringEchoServer::NUM_IO_BUFFERS);
}

static uint8_t* get_buffer_addr(uint8_t* base_addr, uint16_t idx) {
    return base_addr + (idx * UringEchoServer::IO_BUFFER_SIZE);
}

static void recycle_buffer(io_uring_buf_ring* buf_ring, uint8_t* buf_base_addr, uint16_t idx) {
    // https://man7.org/linux/man-pages/man3/io_uring_buf_ring_add.3.html
    // buf_offset is the offset to insert at from the current tail. If just one buffer is
    // provided before the tail is committed with io_uring_buf_ring_advance(3) or
    // io_uring_buf_ring_cq_advance(3), then buf_offset should be 0
    io_uring_buf_ring_add(buf_ring, get_buffer_addr(buf_base_addr, idx), UringEchoServer::IO_BUFFER_SIZE, idx,
                          io_uring_buf_ring_mask(UringEchoServer::NUM_IO_BUFFERS), /* buf_offset */ 0);
    // Make the buffer visible to the kernel
    io_uring_buf_ring_advance(buf_ring, 1);
}

// Context is used to map from SQEs to CQEs using "user_data"
enum class ContextType : uint8_t { Accept, Close, Read, Write };
struct Context {
    int32_t client_fd;
    ContextType type;
    uint16_t buffer_idx;
};

// Context layout (8-bytes)
// | 4-bytes - client_fd | 1-byte - type | 2-bytes - buffer index | 1-byte not used
static void set_context(io_uring_sqe* sqe, ContextType type, int32_t client_fd, uint16_t buffer_idx) {
    // Make sure we can fit our context in io_uring_sqe::user_data/io_uring_cqe::user_data
    static_assert(8 == sizeof(__u64));

    auto* buffer = reinterpret_cast<uint8_t*>(&sqe->user_data);

    // Write client_fd
    *(reinterpret_cast<int32_t*>(buffer)) = client_fd;
    buffer += 4;
    // Write type
    *buffer = static_cast<uint8_t>(type);
    buffer += 1;
    // Write buffer index
    *(reinterpret_cast<uint16_t*>(buffer)) = buffer_idx;
}

static Context get_context(io_uring_cqe* cqe) {
    Context ctx{};
    auto* buffer = reinterpret_cast<uint8_t*>(&cqe->user_data);

    ctx.client_fd = *(reinterpret_cast<int32_t*>(buffer));
    buffer += 4;
    ctx.type = static_cast<ContextType>(*buffer);
    buffer += 1;
    ctx.buffer_idx = *(reinterpret_cast<uint16_t*>(buffer));

    return ctx;
}

static bool flag_is_set(io_uring_cqe* cqe, unsigned flag) { return (cqe->flags & flag); }

// Pretty much a copy-paste from:
// https://github.com/axboe/liburing/blob/master/examples/io_uring-udp.c
static uint8_t* init_buffer_ring(io_uring* ring, io_uring_buf_ring** buf_ring, size_t ring_size) {
    // https://man7.org/linux/man-pages/man3/io_uring_register_buf_ring.3.html
    // The ring_addr field must contain the address to the memory
    // allocated to fit this ring. The memory must be page aligned and
    // hence allocated appropriately using eg posix_memalign(3) or
    // similar. The size of the ring is the product of ring_entries and
    // the size of struct io_uring_buf. ring_entries is the desired
    // size of the ring, and must be a power-of-2 in size. The maximum
    // size allowed is 2^15 (32768). bgid is the buffer group ID
    // associated with this ring. SQEs that select a buffer have a
    // buffer group associated with them in their buf_group field, and
    // the associated CQEs will have IORING_CQE_F_BUFFER set in their
    // flags member, which will also contain the specific ID of the
    // buffer selected. The rest of the fields are reserved and must be
    // cleared to zero.

    // From: https://unixism.net/loti/ref-iouring/io_uring_register.html
    // Currently, the buffers must be anonymous, non-file-backed memory, such as that returned by malloc(3) or
    // mmap(2) with the MAP_ANONYMOUS flag set. It is expected that this limitation will be lifted in the
    // future
    void* ring_addr =
        mmap(/* addr */ nullptr, ring_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    // mmap: If addr is nullptr, then the kernel chooses the (page-aligned) address at which to create the
    // mapping
    if (ring_addr == MAP_FAILED) {
        error(EXIT_ERROR, 0, "mmap ring");
    }

    io_uring_buf_reg reg{};
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = reinterpret_cast<__u64>(ring_addr);
    reg.ring_entries = UringEchoServer::NUM_IO_BUFFERS;
    reg.bgid = BUFFER_GROUP_ID;

    const unsigned flags = 0;
    const int register_buf_ring_result = io_uring_register_buf_ring(ring, &reg, flags);
    if (register_buf_ring_result != 0) {
        error(EXIT_ERROR, -register_buf_ring_result, "io_uring_register_buf_ring");
    }

    *buf_ring = reinterpret_cast<io_uring_buf_ring*>(ring_addr);
    io_uring_buf_ring_init(*buf_ring);

    // Start of the actual buffer memory
    uint8_t* buffer_base_addr = get_buffer_base_addr(ring_addr);

    // Add all buffers to a shared buffer ring
    for (uint16_t buffer_idx = 0u; buffer_idx < UringEchoServer::NUM_IO_BUFFERS; ++buffer_idx) {
        // https://man7.org/linux/man-pages/man3/io_uring_buf_ring_add.3.html
        io_uring_buf_ring_add(*buf_ring, get_buffer_addr(buffer_base_addr, /* bid */ buffer_idx),
                              UringEchoServer::IO_BUFFER_SIZE, buffer_idx,
                              io_uring_buf_ring_mask(UringEchoServer::NUM_IO_BUFFERS),
                              /* buf_offset */ buffer_idx);
    }

    // Make 'count' new buffers visible to the kernel. Called after io_uring_buf_ring_add() has been called
    // 'count' times to fill in new buffers.
    io_uring_buf_ring_advance(*buf_ring, UringEchoServer::NUM_IO_BUFFERS);

    return buffer_base_addr;
}

UringEchoServer::UringEchoServer(int port) {
    listening_socket_ = create_listening_socket(port);
    init_io_uring(&ring_, NUM_SUBMISSION_QUEUE_ENTRIES);
    ring_initialized_ = true;

    // Init the shared IO buffers
    constexpr size_t ring_size = buffer_ring_size();
    buf_ring_size_ = ring_size;
    io_buffers_base_addr_ = init_buffer_ring(&ring_, &buf_ring_, ring_size);
}

UringEchoServer::~UringEchoServer() {
    if (buf_ring_ != nullptr) {
        munmap(buf_ring_, buf_ring_size_);
    }

    if (ring_initialized_) {
        io_uring_queue_exit(&ring_);
    }

    if (listening_socket_ != ERROR) {
        close(listening_socket_);
    }
}

void UringEchoServer::run_event_loop(bool& loop) {
    io_uring_cqe* cqes[CQE_BATCH_SIZE];

    add_accept(); // Start accepting connections
    while (loop) {
        const int result = io_uring_submit_and_wait(&ring_, NUM_WAIT_ENTRIES);
        if (result == -EINTR) { // The system call was interrupted
            continue;
        }
        if (result < 0) {
            // TODO: Maybe not exit here... not sure...
            error(EXIT_ERROR, result, "io_uring_submit_and_wait");
        }

        const unsigned num_cqes = io_uring_peek_batch_cqe(&ring_, cqes, CQE_BATCH_SIZE);
        for (unsigned cqe_idx = 0; cqe_idx < num_cqes; ++cqe_idx) {
            io_uring_cqe* cqe = cqes[cqe_idx];
            const auto ctx = get_context(cqe);

            switch (ctx.type) {
            case ContextType::Accept:
                handle_accept(cqe);
                break;
            case ContextType::Close:
                // No-op
                LOG_INFO("Closed: %d\n", ctx.client_fd);
                break;
            case ContextType::Read:
                handle_read(cqe, ctx.client_fd);
                break;
            case ContextType::Write:
                handle_write(cqe, ctx.client_fd, ctx.buffer_idx);
                break;
            default:
                error(EXIT_ERROR, 0, "context type not handled: %d", static_cast<int>(ctx.type));
                break;
            }
        }
        // Mark the SQEs as handled
        io_uring_cq_advance(&ring_, num_cqes);
    }
}

void UringEchoServer::handle_accept(io_uring_cqe* cqe) {
    const auto client_fd = cqe->res;
    if (client_fd >= 0) {
        // Valid fd, start reading
        add_recv(client_fd);
        LOG_INFO("New connection: %d\n", client_fd);
    } else {
        LOG_ERROR("Accept error: %d\n", client_fd);
    }

    if (!flag_is_set(cqe, IORING_CQE_F_MORE)) {
        // The current accept will not produce any more entries, add a new one
        add_accept();
    }
}

void UringEchoServer::handle_read(io_uring_cqe* cqe, int client_fd) {
    const auto result = cqe->res;
    bool closed = false;

    if (result == 0 || result == -EBADF || result == -ECONNRESET) { // EOF, Broken pipe or Connection reset by peer
        add_close(client_fd);
        closed = true;
    } else if (result < 0) { // Error
        LOG_ERROR("Recv error: %d\n", result);
        if (result == -ENOBUFS) {
            // No buffer to read data into...
        } else {
            add_close(client_fd); // Brute force close for now
            closed = true;
        }
    } else {
        // We read some data. Yay!

        if (!flag_is_set(cqe, IORING_CQE_F_BUFFER)) {
            // No buffer flag set, not sure this can happen(?)...
            add_close(client_fd); // Brute force close for now
            closed = true;
        } else {
            const uint16_t buffer_idx = cqe->flags >> 16;
            const void* addr = get_buffer_addr(io_buffers_base_addr_, buffer_idx);
            LOG_INFO("Read %d bytes on fd: %d\n", result, client_fd);

            // Echo the data we just read
            add_write(client_fd, addr, result, buffer_idx);
        }
    }

    if (!closed && !flag_is_set(cqe, IORING_CQE_F_MORE)) {
        // The current recv will not produce any more entries, add a new one
        add_recv(client_fd);
    }
}

void UringEchoServer::handle_write(io_uring_cqe* cqe, int client_fd, uint16_t buffer_idx) {
    const auto result = cqe->res;
    if (result == -EPIPE || result == -EBADF || result == -ECONNRESET) {
        // EPIPE - Broken pipe
        // ECONNRESET - Connection reset by peer
        // EBADF - Fd has been closed
        add_close(client_fd);
    } else if (result < 0) {
        LOG_ERROR("Write error: %d\n", result);
    }

    // Give the buffer back to io_uring, so it can be re-used
    recycle_buffer(buf_ring_, io_buffers_base_addr_, buffer_idx);
}

io_uring_sqe* UringEchoServer::get_sqe() {
    // returns a pointer to the next submission queue event on success and NULL on failure.
    // If NULL is returned, the SQ ring is currently full and entries must be submitted for processing before
    // new ones can get allocated
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }

    if (sqe == nullptr) {
        error(EXIT_ERROR, 0, "io_uring_get_sqe");
    }

    return sqe;
}

void UringEchoServer::add_accept() {
    io_uring_sqe* sqe = get_sqe();
    set_context(sqe, ContextType::Accept, /* client_fd */ -1, /* buffer_idx */ 0u);
    const int flags = 0;

    // From: https://man7.org/linux/man-pages/man3/io_uring_prep_multishot_accept.3.html
    // Note that for the multishot variants, setting addr and addrlen
    // may not make a lot of sense, as the same value would be used for
    // every accepted connection. This means that the data written to
    // addr may be overwritten by a new connection before the
    // application has had time to process a past connection.
    // TODO: Maybe not use addr and addrlen... We currently pass them here, but never read them
    io_uring_prep_multishot_accept(sqe, listening_socket_, (sockaddr*)&client_addr_, &client_addr_len_,
                                   flags);
}

void UringEchoServer::add_close(int client_fd) {
    io_uring_sqe* sqe = get_sqe();
    set_context(sqe, ContextType::Close, client_fd, /* buffer_idx */ 0u);
    io_uring_prep_close(sqe, client_fd);
}

void UringEchoServer::add_recv(int client_fd) {
    io_uring_sqe* sqe = get_sqe();
    set_context(sqe, ContextType::Read, client_fd, /* buffer_idx */ 0u);
    io_uring_prep_recv_multishot(sqe, client_fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BUFFER_GROUP_ID;
}

void UringEchoServer::add_write(int client_fd, const void* data, unsigned length, uint16_t buffer_idx) {
    io_uring_sqe* sqe = get_sqe();
    set_context(sqe, ContextType::Write, client_fd, buffer_idx);
    io_uring_prep_write(sqe, client_fd, data, length, 0);
}
