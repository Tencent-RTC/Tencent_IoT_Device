if(ESP_PLATFORM)
  return()
endif()

set(TARGET_APP_NAME iot_video_demo)

set(ROOT_DIR ${CMAKE_SOURCE_DIR})

# tc_iot_sdk 静态库由 src/iot/CMakeLists.txt 创建。
# demo 可执行文件 —— 只依赖 public API：include 仅 {当前目录, src/api/iot}，
# 链接预编译并已自包含的 tc_iot_sdk .a（build.sh 先编 .a、ar 合并后经
# -DTC_IOT_SDK_LIB_PATH 传入）。未设该变量时不建 demo（如仅产 .a 的首次配置）。
if(NOT BUILD_IOT_AV)
  return()
endif()
if(NOT TC_IOT_SDK_LIB_PATH)
  message(STATUS "Demo: TC_IOT_SDK_LIB_PATH not set, skip iot_video_demo")
  return()
endif()

# minimp4 单头文件库：在 build 目录中生成 .c 文件，内含 #define MINIMP4_IMPLEMENTATION
set(MINIMP4_C ${CMAKE_CURRENT_BINARY_DIR}/minimp4.c)
file(WRITE ${MINIMP4_C}
    "#define MINIMP4_IMPLEMENTATION\n"
    "#include \"minimp4.h\"\n"
)

list(APPEND IOT_VIDEO_SRC_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/iot_demo.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cmd_exec.c
    ${CMAKE_CURRENT_SOURCE_DIR}/file_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/av_device_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/av_sender.c
    ${CMAKE_CURRENT_SOURCE_DIR}/av_receiver.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cloud_storage_demo.c
    ${MINIMP4_C}
)

add_executable(${TARGET_APP_NAME} ${IOT_VIDEO_SRC_LIST})

# 窄 include：任何 src/base、src/third_party 越界 include 都会在此编译失败（强制只依赖 public API）。
target_include_directories(${TARGET_APP_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ROOT_DIR}/src/api/iot
    ${ROOT_DIR}/src/base/hal
    ${ROOT_DIR})

if(WIN32)
    target_link_libraries(${TARGET_APP_NAME} PRIVATE ${TC_IOT_SDK_LIB_PATH} Ws2_32.lib)
elseif(APPLE)
    target_link_libraries(${TARGET_APP_NAME} PRIVATE ${TC_IOT_SDK_LIB_PATH} z m pthread)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    target_link_libraries(${TARGET_APP_NAME} PRIVATE ${FOUNDATION_FRAMEWORK})
    find_library(SYSTEM_CONFIGURATION_FRAMEWORK SystemConfiguration)
    target_link_libraries(${TARGET_APP_NAME} PRIVATE ${SYSTEM_CONFIGURATION_FRAMEWORK})
    if(NOT IOS)
        find_library(APPKIT_FRAMEWORK AppKit)
        target_link_libraries(${TARGET_APP_NAME} PRIVATE ${APPKIT_FRAMEWORK})
    endif()
else()
    # 系统库：桌面用系统 z(libz)+dl；交叉编译时 zlib 已并入 .a、dl 在 musl/uclibc 并入 libc，仅留 m+pthread。
    set(_demo_syslibs m pthread)
    if(NOT CMAKE_CROSSCOMPILING)
        list(APPEND _demo_syslibs z dl)
    endif()
    if(USE_TRTC_C AND ${BUILD_TRTC} EQUAL 1)
        # c 模式：.a 已并入 TRTC-c/opus，仅需系统库；--start-group 兜底归并库内部互相引用。
        target_link_libraries(${TARGET_APP_NAME} PRIVATE
            -Wl,--start-group ${TC_IOT_SDK_LIB_PATH} -Wl,--end-group ${_demo_syslibs})
    elseif(CMAKE_CROSSCOMPILING)
        # cc 交叉：默认静态 libliteav.a 已并入自包含 .a；若存在 .so（如 x5 动态 cc）则单独链接。
        if(TRTC_CC_LIB_ARCH)
            set(_trtc_lib_dir ${ROOT_DIR}/src/third_party/iot_trtc/lib/${TRTC_CC_LIB_ARCH})
        else()
            set(_trtc_lib_dir ${ROOT_DIR}/src/third_party/iot_trtc/lib/x64)
        endif()
        set(_liteav_so ${_trtc_lib_dir}/libliteav.so)
        set(_txffmpeg_so ${_trtc_lib_dir}/libtxffmpeg.so)
        if(EXISTS ${_liteav_so} AND EXISTS ${_txffmpeg_so})
            set(_cc_extra stdc++)
            set(_zlib_link "")
            if(DEFINED XC_LINK_DIRS AND EXISTS ${XC_LINK_DIRS}/libz.so.1.2.11)
                set(_zlib_link ${XC_LINK_DIRS}/libz.so.1.2.11)
            elseif(DEFINED XC_LINK_DIRS AND EXISTS ${XC_LINK_DIRS}/libz.so.1)
                set(_zlib_link ${XC_LINK_DIRS}/libz.so.1)
            else()
                list(APPEND _demo_syslibs z)
            endif()
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "mips")
                list(APPEND _cc_extra atomic)
            endif()
            target_link_libraries(${TARGET_APP_NAME} PRIVATE
                -Wl,--start-group ${TC_IOT_SDK_LIB_PATH} -Wl,--end-group
                ${_liteav_so} ${_txffmpeg_so} ${_cc_extra} ${_zlib_link} ${_demo_syslibs})
        else()
            # MIPS32 无原生 64-bit 原子操作，需 libatomic。
            set(_cc_extra stdc++)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "mips")
                list(APPEND _cc_extra atomic)
            endif()
            target_link_libraries(${TARGET_APP_NAME} PRIVATE
                -Wl,--start-group ${TC_IOT_SDK_LIB_PATH} -Wl,--end-group ${_cc_extra} ${_demo_syslibs})
        endif()
    else()
        # cc x64 原生：TRTC 走预编 libliteav.so/libtxffmpeg.so。
        find_library(LIBLITEAV libliteav.so ${ROOT_DIR}/src/third_party/iot_trtc/lib/x64/ NO_DEFAULT_PATH)
        find_library(LIBTXFFMPEG libtxffmpeg.so ${ROOT_DIR}/src/third_party/iot_trtc/lib/x64/ NO_DEFAULT_PATH)
        target_link_libraries(${TARGET_APP_NAME} PRIVATE
            -Wl,--start-group ${TC_IOT_SDK_LIB_PATH} -Wl,--end-group ${LIBLITEAV} ${LIBTXFFMPEG} ${_demo_syslibs})
    endif()
endif()
