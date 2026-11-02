cmake_minimum_required(VERSION 3.24)
project(hello LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(
  mapkern
  URL https://github.com/grangedevgroup-code/mapkern-native/archive/refs/tags/v0.1.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(mapkern)

add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE mapkern_window)
