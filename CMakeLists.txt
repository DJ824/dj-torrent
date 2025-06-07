cmake_minimum_required(VERSION 3.30)
project(dj_torrent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Remove verbose makefile for cleaner output
# set(CMAKE_VERBOSE_MAKEFILE ON)

if(APPLE)
    set(POSSIBLE_OPENSSL_ROOTS
            "/opt/homebrew"
            "/usr/local"
            "/opt/local"
    )

    foreach(ROOT ${POSSIBLE_OPENSSL_ROOTS})
        if(EXISTS "${ROOT}/include/openssl/ssl.h")
            set(OPENSSL_ROOT_DIR "${ROOT}")
            message(STATUS "Found OpenSSL at: ${ROOT}")
            break()
        endif()
    endforeach()

    if(NOT OPENSSL_ROOT_DIR)
        message(FATAL_ERROR "Could not find OpenSSL. Try: brew install openssl")
    endif()
endif()

find_package(OpenSSL REQUIRED)

# Fix the relative paths - use current directory
add_executable(dj_torrent
        main.cpp
        torrent_file.cpp
        tracker.cpp
        bencode_parser.cpp
        piece_manager.cpp
        peer.cpp
        peer_connection.cpp
        peer_manager.cpp
        bencode_parser.h
        torrent_file.h
        piece_manager.h
        peer.h
        peer_connection.h
        peer_manager.h
        tracker.h
        dht_node.cpp
        dht_node.h
)

target_link_libraries(dj_torrent
        PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_options(dj_torrent PRIVATE
        -Wall
        -Wextra
        -g
)

message(STATUS "OpenSSL_FOUND: ${OPENSSL_FOUND}")
message(STATUS "OPENSSL_VERSION: ${OPENSSL_VERSION}")
message(STATUS "OPENSSL_INCLUDE_DIR: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OPENSSL_CRYPTO_LIBRARY: ${OPENSSL_CRYPTO_LIBRARY}")
message(STATUS "OPENSSL_SSL_LIBRARY: ${OPENSSL_SSL_LIBRARY}")