cmake_minimum_required(VERSION 3.0)
project(LINENOISE)

add_library(linenoise STATIC
  linenoise.c
)

target_include_directories(linenoise PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
