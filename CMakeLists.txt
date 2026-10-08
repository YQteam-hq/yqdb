cmake_minimum_required(VERSION 3.10)
project(yqdb C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

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
    src/yq_json.c
    src/yq_batch.c
    src/yq_index.c
    src/yq_ttl.c
    src/yq_compress.c
    src/yq_crypto.c
    src/yq_pubsub.c
    src/yq_backup.c
    src/yq_cluster.c
    src/yq_cache.c
    src/yq_web.c
    src/yq_web_missing_functions.c
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
