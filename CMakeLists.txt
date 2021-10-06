cmake_minimum_required(VERSION 3.5)
project(Task-Spooler C)

set(CMAKE_C_STANDARD 11)

set(target ts)
add_executable(
        ${target}
        client.c
        env.c
        error.c
        execute.c
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
