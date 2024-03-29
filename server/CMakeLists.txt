cmake_minimum_required(VERSION 3.10)

project(
  julee
  VERSION 0.1
  DESCRIPTION "JULEE server"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_BUILD_TYPE RelWithDebInfo)
#set(CMAKE_BUILD_TYPE Release)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_library(lfcq STATIC
    circular_queue.cpp
)

add_executable(${CMAKE_PROJECT_NAME}_kv src/cceh.cpp Logger.cpp KV.cpp test_KV.cpp)
add_executable(${CMAKE_PROJECT_NAME}_server src/cceh.cpp Logger.cpp KV.cpp rdma_svr.cpp)

target_compile_definitions(${CMAKE_PROJECT_NAME}_kv PUBLIC KV_DEBUG DCCEH)
target_include_directories(${CMAKE_PROJECT_NAME}_kv PUBLIC ${CMAKE_SOURCE_DIR}/)
target_link_libraries(${CMAKE_PROJECT_NAME}_kv pthread rdmacm ibverbs pmemobj numa pmem ssl crypto)

target_compile_definitions(${CMAKE_PROJECT_NAME}_server PUBLIC KV_DEBUG TWOSIDED DCCEH)
target_include_directories(${CMAKE_PROJECT_NAME}_server PUBLIC ${CMAKE_SOURCE_DIR}/)
target_link_libraries(${CMAKE_PROJECT_NAME}_server pthread rdmacm ibverbs pmemobj numa pmem ssl crypto)
