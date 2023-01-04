cmake_minimum_required(VERSION 3.24)
project(UringEchoServer)

set(CMAKE_CXX_STANDARD 17)

# Flags used regardless of build type
set(DEFAULT_COMPILER_FLAGS -Wall -Wextra -Wpedantic -Werror -g)

include_directories(src)
file(GLOB SRC_FILES src/net/*.cpp)
add_executable(uring_echo_server src/server.cpp ${SRC_FILES})
target_link_libraries(uring_echo_server PRIVATE uring)

if (CMAKE_BUILD_TYPE MATCHES Debug)
    add_compile_definitions(LOGGING_ENABLED)
    #add_compile_definitions(PRINT_IO_URING_PROBE_RESULT)

    target_compile_options(uring_echo_server PRIVATE ${DEFAULT_COMPILER_FLAGS})
elseif (CMAKE_BUILD_TYPE MATCHES Release)
    target_compile_options(uring_echo_server PRIVATE ${DEFAULT_COMPILER_FLAGS} -O3)
    # Enable LTO
    set_property(TARGET uring_echo_server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else ()
    message(FATAL_ERROR Unknown build type ${CMAKE_BUILD_TYPE})
endif ()
