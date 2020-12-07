cmake_minimum_required(VERSION 3.10)
project(Caching)
set( CMAKE_VERBOSE_MAKEFILE on )
find_package(PkgConfig)
pkg_check_modules(LIBCONFIG_PLUSPLUS libconfig REQUIRED)
link_libraries(hashing /usr/local/Cellar/libconfig/1.7.2/lib/libconfig++.a)

# Sources
include_directories(include)
##### Offline Policies #####
add_executable(cache_belady src/cache_belady.cpp)
add_executable(cache_belady_aggdelay src/cache_belady_aggdelay.cpp)
##### Online Policies #####
add_executable(cache_lru src/cache_lru.cpp)
add_executable(cache_arc src/cache_arc.cpp)
add_executable(cache_lhd src/cache_lhd.cpp)
add_executable(cache_lru_aggdelay src/cache_lru_aggdelay.cpp)
add_executable(cache_arc_aggdelay src/cache_arc_aggdelay.cpp)
add_executable(cache_lhd_aggdelay src/cache_lhd_aggdelay.cpp)
