#include <csignal>
#include <cstdio>
#include <sys/utsname.h>

#include "net/UringEchoServer.h"
#include "util/util.h"

static const char* IO_URING_OP_NAMES[] = {
    "IORING_OP_NOP",
    "IORING_OP_READV",
    "IORING_OP_WRITEV",
    "IORING_OP_FSYNC",
    "IORING_OP_READ_FIXED",
    "IORING_OP_WRITE_FIXED",
    "IORING_OP_POLL_ADD",
    "IORING_OP_POLL_REMOVE",
    "IORING_OP_SYNC_FILE_RANGE",
    "IORING_OP_SENDMSG",
    "IORING_OP_RECVMSG",
    "IORING_OP_TIMEOUT",
    "IORING_OP_TIMEOUT_REMOVE",
    "IORING_OP_ACCEPT",
    "IORING_OP_ASYNC_CANCEL",
    "IORING_OP_LINK_TIMEOUT",
    "IORING_OP_CONNECT",
    "IORING_OP_FALLOCATE",
    "IORING_OP_OPENAT",
    "IORING_OP_CLOSE",
    "IORING_OP_FILES_UPDATE",
    "IORING_OP_STATX",
    "IORING_OP_READ",
    "IORING_OP_WRITE",
    "IORING_OP_FADVISE",
    "IORING_OP_MADVISE",
    "IORING_OP_SEND",
    "IORING_OP_RECV",
    "IORING_OP_OPENAT2",
    "IORING_OP_EPOLL_CTL",
    "IORING_OP_SPLICE",
    "IORING_OP_PROVIDE_BUFFERS",
    "IORING_OP_REMOVE_BUFFERS",
    "IORING_OP_TEE",
    "IORING_OP_SHUTDOWN",
    "IORING_OP_RENAMEAT",
    "IORING_OP_UNLINKAT",
    "IORING_OP_MKDIRAT",
    "IORING_OP_SYMLINKAT",
    "IORING_OP_LINKAT",
    "IORING_OP_MSG_RING",
    "IORING_OP_FSETXATTR",
    "IORING_OP_SETXATTR",
    "IORING_OP_FGETXATTR",
    "IORING_OP_GETXATTR",
    "IORING_OP_SOCKET",
    "IORING_OP_URING_CMD",
    "IORING_OP_SEND_ZC",
    "IORING_OP_SENDMSG_ZC",
};

bool running = true;

static void no_op_sig_handler(int) {}
static void sig_handler(int s) {
    printf("Signal %d received. Shutting down...\n", s);
    running = false;
}

constexpr int PORT = 6379;

int main() {
    utsname u{};
    uname(&u);
    printf("Kernel version: %s\n", u.release);

#ifdef PRINT_IO_URING_PROBE_RESULT
    io_uring_probe* probe = io_uring_get_probe();
    printf("Supported io_uring operations:\n");
    for (int i = 0; i < IORING_OP_LAST; i++) {
        const char* supported = (io_uring_opcode_supported(probe, i) ? "yes" : "no");
        printf("%s: %s\n", IO_URING_OP_NAMES[i], supported);
    }
#endif

    UNUSED(IO_URING_OP_NAMES);

    signal(SIGPIPE, no_op_sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    UringEchoServer server(PORT);
    printf("Starting uring server on port: %d\n", PORT);
    server.run_event_loop(running);
    return 0;
}