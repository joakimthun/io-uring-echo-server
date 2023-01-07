#include <csignal>
#include <cstdio>
#include <sys/utsname.h>

#include "net/EpollEchoServer.h"

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

    signal(SIGPIPE, no_op_sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    EpollEchoServer server(PORT);
    printf("Starting epoll server on port: %d\n", PORT);
    server.run_event_loop(running);

    return 0;
}