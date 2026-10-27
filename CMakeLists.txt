cmake_minimum_required(VERSION 3.13)
project(stlv C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(STLV_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(STLV_SANITIZE "Build tests with ASan/UBSan" ON)

set(STLV_DEFINITIONS "" CACHE STRING
    "sTLV 配置宏，例如 STLV_LEN_SIZE=2;STLV_MAX_PAYLOAD=512；会传给所有链接 stlv 的目标")

add_library(stlv STATIC src/stlv.c)
target_include_directories(stlv PUBLIC include)
# PUBLIC：保证库和使用者看到相同的配置（stlv_init 也会在运行时校验）
target_compile_definitions(stlv PUBLIC ${STLV_DEFINITIONS})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(stlv PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
endif()

if(STLV_BUILD_TESTS)
    enable_testing()
    add_executable(stlv_example examples/loopback.c)
    target_link_libraries(stlv_example PRIVATE stlv)
    add_test(NAME example COMMAND stlv_example)

    # 名称:TYPE_SIZE:LEN_SIZE:MAX_PAYLOAD:CRC_TABLE
    set(STLV_TEST_MATRIX
        "t1l1:1:1:256:0"
        "t1l2:1:2:1024:1"
        "t2l1:2:1:64:0"
        "t2l2:2:2:600:1"
        "t1l2max:1:2:65535:1"
        "t1l1min:1:1:32:0")
    foreach(cfg IN LISTS STLV_TEST_MATRIX)
        string(REPLACE ":" ";" cfg_list "${cfg}")
        list(GET cfg_list 0 name)
        list(GET cfg_list 1 tsize)
        list(GET cfg_list 2 lsize)
        list(GET cfg_list 3 maxp)
        list(GET cfg_list 4 table)
        set(exe stlv_test_${name})
        add_executable(${exe}
            src/stlv.c test/test_main.c test/test_codec.c test/test_rx.c test/test_fault.c)
        target_include_directories(${exe} PRIVATE include test)
        target_compile_definitions(${exe} PRIVATE
            STLV_TYPE_SIZE=${tsize} STLV_LEN_SIZE=${lsize}
            STLV_MAX_PAYLOAD=${maxp} STLV_CRC_TABLE=${table})
        target_compile_options(${exe} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        if(STLV_SANITIZE)
            target_compile_options(${exe} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
            target_link_options(${exe} PRIVATE -fsanitize=address,undefined)
        endif()
        add_test(NAME ${name} COMMAND ${exe})
        if(maxp GREATER 4096)
            # 大帧配置数据量大，减少故障注入帧数以控制耗时
            set_tests_properties(${name} PROPERTIES ENVIRONMENT STLV_FAULT_FRAMES=300)
        endif()
    endforeach()
endif()
