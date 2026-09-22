cmake_minimum_required(VERSION 3.16)
project(acgc_android C CXX)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC  ON CACHE BOOL "" FORCE)
set(SDL_TEST    OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_PATH} SDL)

add_subdirectory(${PC_DIR} pc)