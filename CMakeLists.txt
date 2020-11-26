cmake_minimum_required(VERSION 3.5)
project(Task-Spooler C)

set(CMAKE_C_STANDARD 11)

set(CMAKE_CUDA_COMPILER $ENV{CUDA_HOME}/bin/nvcc)
find_package(CUDA REQUIRED)
include_directories($ENV{CUDA_HOME}/include)

add_executable(
        ts
        client.c
        env.c
        error.c
        execute.c
        gpu.c
        info.c
        jobs.c
        list.c
        mail.c
        main.c
        msg.c
        msgdump.c
        print.c
        server.c
        server_start.c
        signals.c
        tail.c
)

target_link_libraries(ts ${CUDA_LIBRARIES})

set(
        TS_PERMISSIONS
        OWNER_WRITE OWNER_READ OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
)

install(TARGETS ts DESTINATION /usr/local/bin)
