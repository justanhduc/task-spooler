cmake_minimum_required(VERSION 3.5)
project(Task-Spooler C)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

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
        feishu.c
        feishu_card.c
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
target_link_libraries(${target} CURL::libcurl Threads::Threads)

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
  target_compile_definitions(${target} PRIVATE CPU)
  target_compile_definitions(makeman PRIVATE CPU)
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

option(BUILD_TESTING "Build isolated regression tests" OFF)
include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(TEST_SOURCES ${TASK_SPOOLER_SOURCES})
  list(REMOVE_ITEM TEST_SOURCES gpu.c)
  add_executable(ts_gpu_test main.c ${TEST_SOURCES} gpu.c tests/fake_nvml/fake_nvml.c)
  target_include_directories(ts_gpu_test PRIVATE tests/fake_nvml)
  target_link_libraries(ts_gpu_test CURL::libcurl Threads::Threads)
  add_executable(test_card tests/card_fixture.c feishu_card.c cjson/cJSON.c)
  target_include_directories(test_card PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME regression
    COMMAND ${CMAKE_COMMAND} -E env
      TS_BINARY=$<TARGET_FILE:ts>
      TS_GPU_TEST_BINARY=$<TARGET_FILE:ts_gpu_test>
      TS_CARD_TEST_BINARY=$<TARGET_FILE:test_card>
      ${Python3_EXECUTABLE} -m unittest discover -s tests -v)
  set_tests_properties(regression PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} TIMEOUT 120)
endif()
