cmake_minimum_required(VERSION 3.22)
project(UringEchoServer)

set(CMAKE_CXX_STANDARD 17)

# Flags used regardless of build type
set(DEFAULT_COMPILER_FLAGS -Wall -Wextra -Wpedantic -Werror -g)

include_directories(src)

add_executable(uring_echo_server src/uring_echo_server.cpp src/net/UringEchoServer.cpp)
target_link_libraries(uring_echo_server PRIVATE uring)

add_executable(epoll_echo_server src/epoll_echo_server.cpp src/net/EpollEchoServer.cpp)

# INLINE_EPOLL_WRITE - A "best effort" event loop.
# We just try to echo what we just read and will drop the data on EAGAIN/EWOULDBLOCK or other errors
# and partial writes. The benefit is that we do not need any buffer management for writes...

if (CMAKE_BUILD_TYPE MATCHES Debug)
    add_compile_definitions(DEBUG)
    add_compile_definitions(LOGGING_ENABLED)
    #add_compile_definitions(PRINT_IO_URING_PROBE_RESULT)
    add_compile_definitions(INLINE_EPOLL_WRITE)

    target_compile_options(uring_echo_server PRIVATE ${DEFAULT_COMPILER_FLAGS})
elseif (CMAKE_BUILD_TYPE MATCHES Release)
    add_compile_definitions(INLINE_EPOLL_WRITE)

    target_compile_options(uring_echo_server PRIVATE ${DEFAULT_COMPILER_FLAGS} -O3)
    # Enable LTO
    set_property(TARGET uring_echo_server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else ()
    message(FATAL_ERROR Unknown build type ${CMAKE_BUILD_TYPE})
endif ()
