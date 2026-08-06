cmake_minimum_required(VERSION 3.5)
project(Task-Spooler C)

set(CMAKE_C_STANDARD 11)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Enables the GPU-scheduling features. These only need NVML (which ships with the
# NVIDIA driver), not the full CUDA compute toolkit -- see cmake/FindNVML.cmake.
# The name is kept as-is for backward compatibility with existing build scripts.
option(TASK_SPOOLER_COMPILE_CUDA "Compile GPU (NVML) support" ON)

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
  # Depend on NVML only (part of the NVIDIA driver), not the full CUDA Toolkit.
  find_package(NVML REQUIRED)
  target_link_libraries(${target} NVML::NVML)
else(TASK_SPOOLER_COMPILE_CUDA)
  message("Installing a CPU version...")
  add_definitions(-DCPU)
endif(TASK_SPOOLER_COMPILE_CUDA)

# Man pages
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/man1)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/man1/ts.1
  COMMAND ${CMAKE_CURRENT_BINARY_DIR}/makeman
  DEPENDS makeman
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/man1)
add_custom_target(man ALL
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/man1/ts.1)

# install
install(TARGETS ${target}
        RUNTIME)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/man1
        TYPE MAN)
