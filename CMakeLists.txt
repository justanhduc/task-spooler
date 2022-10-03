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
        tail.c
        cjson/cJSON.c)

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
  if(${CMAKE_VERSION} VERSION_LESS "3.17.0") 
    find_package(CUDA REQUIRED)

    # Do what the new package does
    find_library(CUDA_DRIVER_LIBRARY
                NAMES cuda_driver cuda
                HINTS ${CUDA_TOOLKIT_ROOT_DIR}
                      ENV CUDA_PATH
                PATH_SUFFIXES nvidia/current lib64 lib/x64 lib)
    if (NOT CUDA_DRIVER_LIBRARY)
        # Don't try any stub directories until we have exhausted all other search locations.
        find_library(CUDA_DRIVER_LIBRARY
                    NAMES cuda_driver cuda
                    HINTS ${CUDA_TOOLKIT_ROOT_DIR}
                          ENV CUDA_PATH
                    PATH_SUFFIXES lib64/stubs lib/x64/stubs lib/stubs stubs)
    endif ()
    mark_as_advanced(CUDA_DRIVER_LIBRARY)
    ##

    target_link_libraries(${target} nvidia-ml)
  else()
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(${target} CUDA::nvml)
  endif()
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
