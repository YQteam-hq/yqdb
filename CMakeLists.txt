cmake_minimum_required(VERSION 3.10)
project(yqdb C)

# Required for add_test()/ctest to pick up any test at all; without it
# `ctest --test-dir build` reports "No tests were found" and exits 0, so CI
# passes without ever running the suite.
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

# 没有 enable_testing() 时 add_test() 不会生成 CTestTestfile.cmake，
# ctest 会报 "No test configuration file found!" 而一个测试都不跑。
enable_testing()

set(SOURCES
    src/yq_enc.c
    src/yq_memblk.c
    src/yq_vfs.c
    src/yq_slice.c
    src/yq_btree.c
    src/yq_memtable.c
    src/yq_wal.c
    src/yq_mvcc.c
    src/yq_recover.c
    src/yq.c
    src/yq_mempool.c
)

add_library(yqdb STATIC ${SOURCES})

target_compile_options(yqdb PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

add_executable(yq_test_basic src/yq_test_basic.c)
target_link_libraries(yq_test_basic yqdb)
add_test(NAME basic_test COMMAND yq_test_basic)

add_executable(yq_test_lifecycle src/yq_test_lifecycle.c)
target_link_libraries(yq_test_lifecycle yqdb)
add_test(NAME lifecycle_test COMMAND yq_test_lifecycle)

add_executable(yq_test_integration src/yq_test_integration.c)
target_link_libraries(yq_test_integration yqdb)
add_test(NAME integration_test COMMAND yq_test_integration)

add_executable(yq_test_readonly src/yq_test_readonly.c)
target_link_libraries(yq_test_readonly yqdb)
add_test(NAME readonly_test COMMAND yq_test_readonly)
add_executable(yq_test_wal src/yq_test_wal.c)
target_link_libraries(yq_test_wal yqdb)
add_test(NAME wal_test COMMAND yq_test_wal)
add_executable(yq_test_opts src/yq_test_opts.c)
target_link_libraries(yq_test_opts yqdb)
add_test(NAME opts_test COMMAND yq_test_opts)

add_executable(yq_test_seek src/yq_test_seek.c)
target_link_libraries(yq_test_seek yqdb)
add_test(NAME seek_test COMMAND yq_test_seek)

add_executable(yq_test_mvcc src/yq_test_mvcc.c)
target_link_libraries(yq_test_mvcc yqdb)
add_test(NAME mvcc_test COMMAND yq_test_mvcc)

# The test binaries report failures through assert(). Release-like build types
# define NDEBUG, which compiles every assertion away and lets the suite "pass"
# without verifying anything, so keep assertions enabled for test targets.
foreach(yq_test_target yq_test_basic yq_test_integration yq_test_seek yq_test_mvcc)
    if(MSVC)
        target_compile_options(${yq_test_target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${yq_test_target} PRIVATE -UNDEBUG)
    endif()
endforeach()
add_executable(yq_test_p4 src/yq_test_p4.c)
target_link_libraries(yq_test_p4 yqdb)
add_test(NAME p4_recovery_test COMMAND yq_test_p4)

add_executable(yq_test_cursor_seek src/yq_test_cursor_seek.c)
target_link_libraries(yq_test_cursor_seek yqdb)
add_test(NAME cursor_seek_test COMMAND yq_test_cursor_seek)
