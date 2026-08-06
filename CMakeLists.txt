cmake_minimum_required(VERSION 3.18.0)
project(zone-server-h2o)

# Fil-C toolchain: build with -DCMAKE_TOOLCHAIN_FILE=cmake/fil-c-toolchain.cmake
# once the Fil-C build is vendored (task #3 in the consolidation plan).
# Stock clang/gcc works for the pre-Fil-C spike (transport + basic ZoneTick).

find_library(H2O_LIB h2o-evloop REQUIRED)
find_library(Z_LIB z REQUIRED)
find_library(SSL_LIB ssl REQUIRED)
find_library(CRYPTO_LIB crypto REQUIRED)
find_library(FDB_C_LIB fdb_c REQUIRED)

find_path(H2O_INCLUDE h2o.h REQUIRED)
find_path(OPENSSL_INCLUDE openssl/ssl.h REQUIRED)
find_path(FDB_INCLUDE foundationdb/fdb_c.h REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(COMMON_OPTIONS -flto=auto -pthread -DFDB_API_VERSION=730)
set(WARNING_OPTIONS -pedantic -Wall -Wextra)

# NOTE: include_directories()/add_compile_options() were global here
# until the first real CI build (task #17) surfaced why that is wrong:
# they leak into every target added afterward in this scope, including
# the vendored picoquic_vendored (same-scope include()) and mujoco
# (child add_subdirectory()) targets in cmake/picoquic.cmake and
# cmake/mujoco.cmake. Our -Wextra leaking into mujoco's build, combined
# with mujoco's own unconditional -Werror (cmake/MujocoOptions.cmake),
# turned mujoco's own pre-existing warnings into fatal build errors --
# not our code's fault, but our leaky CMake structure's fault. Include
# dirs and warning flags are now scoped to our own target only, below.

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_FORTIFY_SOURCE=2")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -O3")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(cmake/picoquic.cmake)
include(cmake/mujoco.cmake)

file(GLOB_RECURSE HANDLER_SOURCES "src/handlers/*.c")
file(GLOB SOURCES "src/*.c")
file(GLOB TRANSPORT_SOURCES "src/transport/*.c")
file(GLOB GEN_SOURCES "src/gen/*.c")
file(GLOB PHYSICS_SOURCES "src/physics/*.c")

add_executable(${PROJECT_NAME} ${HANDLER_SOURCES} ${SOURCES} ${TRANSPORT_SOURCES} ${GEN_SOURCES} ${PHYSICS_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE
    src ${CMAKE_BINARY_DIR} ${H2O_INCLUDE} ${OPENSSL_INCLUDE} ${FDB_INCLUDE})
target_compile_definitions(${PROJECT_NAME} PRIVATE H2O_USE_LIBUV=0)
target_compile_options(${PROJECT_NAME} PRIVATE ${COMMON_OPTIONS} ${WARNING_OPTIONS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_OPTIONS})
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${H2O_LIB} ${FDB_C_LIB} ${SSL_LIB} ${CRYPTO_LIB} ${Z_LIB} picoquic_vendored mujoco m)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
