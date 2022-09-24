cmake_minimum_required(VERSION 3.5)
project(Task-Spooler C)

set(CMAKE_C_STANDARD 11)

option(TASK_SPOOLER_COMPILE_CUDA "Compile CUDA support (NVML)" ON)

# VERSIONING
execute_process(
        COMMAND git rev-parse --is-inside-work-tree
        OUTPUT_VARIABLE GIT_REPO OUTPUT_STRIP_TRAILING_WHITESPACE
)

if (GIT_REPO)
    execute_process (
            COMMAND bash -c "echo $(git describe --dirty --always --tags) | tr - +"
            OUTPUT_VARIABLE git_version
    )
    add_definitions(-DTS_VERSION=${git_version})
endif()


set(target ts)

set(TASK_SPOOLER_SOURCES
        client.c
        env.c
        error.c
        execute.c
        info.c
        jobs.c
        list.c
        mail.c
        msg.c
        msgdump.c
        print.c
        server.c
        server_start.c
        signals.c
        tail.c)

if(TASK_SPOOLER_COMPILE_CUDA)
  set(TASK_SPOOLER_SOURCES ${TASK_SPOOLER_SOURCES} gpu.c)
endif(TASK_SPOOLER_COMPILE_CUDA)

add_executable(
   ${target}
   main.c
   ${TASK_SPOOLER_SOURCES}
)

add_executable(makeman man.c)

if(TASK_SPOOLER_COMPILE_CUDA)
  find_package(CUDAToolkit)
  target_link_libraries(${target} CUDA::nvml)
else(TASK_SPOOLER_COMPILE_CUDA)
  message("Installing a CPU version...")
  add_definitions(-DCPU)
endif(TASK_SPOOLER_COMPILE_CUDA)

# install
install(CODE "execute_process(COMMAND install -c -d /usr/local/bin)")
install(CODE "execute_process(COMMAND install -c ts /usr/local/bin)")
install(CODE "execute_process(COMMAND ./makeman)")
install(CODE "execute_process(COMMAND install -c -d /usr/local/share/man/man1)")
install(CODE "execute_process(COMMAND install -c -m 644 ts.1 /usr/local/share/man/man1)")
