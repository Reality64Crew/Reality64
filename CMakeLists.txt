cmake_minimum_required(VERSION 3.15)
project(Reality64 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Reality64
    source/Main.cpp
)

target_include_directories(Reality64 PRIVATE
    source
)
