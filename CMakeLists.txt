cmake_minimum_required(VERSION 3.10)
project(yqdb C)

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
)

add_library(yqdb STATIC ${SOURCES})

target_compile_options(yqdb PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

add_executable(yq_test_basic src/yq_test_basic.c)
target_link_libraries(yq_test_basic yqdb)
add_test(NAME basic_test COMMAND yq_test_basic)

add_executable(yq_test_integration src/yq_test_integration.c)
target_link_libraries(yq_test_integration yqdb)
add_test(NAME integration_test COMMAND yq_test_integration)

add_executable(yq_test_seek src/yq_test_seek.c)
target_link_libraries(yq_test_seek yqdb)
add_test(NAME seek_test COMMAND yq_test_seek)
