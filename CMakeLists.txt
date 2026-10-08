cmake_minimum_required(VERSION 3.10)
project(yqdb C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

# 没有 enable_testing() 时 add_test() 不会生成 CTestTestfile.cmake，
# ctest 会报 "No test configuration file found!" 而一个测试都不跑。
enable_testing()

# 功能开关必须与 include/yq.h 的默认值一致。
# 下面这些模块被无条件编进库里（src/yq_json.c 也没有条件编译守卫），
# 因此对应的 YQ_ENABLE_* 必须全部为 1，否则会出现"库里有符号、头文件里
# 没有声明"的割裂。要关闭某个模块，需同时改这里、改 yq.h 默认值、
# 并从下面的 SOURCES 列表里删掉对应源文件。
add_compile_definitions(
    YQ_ENABLE_JSON=1
    YQ_ENABLE_BATCH=1
    YQ_ENABLE_INDEX=1
    YQ_ENABLE_TTL=1
    YQ_ENABLE_COMPRESS=1
    YQ_ENABLE_CRYPTO=1
    YQ_ENABLE_PUBSUB=1
    YQ_ENABLE_BACKUP=1
    YQ_ENABLE_CLUSTER=1
    YQ_ENABLE_CACHE=1
    YQ_ENABLE_WEB=1
)

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
