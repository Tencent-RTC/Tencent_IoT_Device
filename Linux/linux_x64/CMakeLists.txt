# Tencent IoT SDK —— Linux 客户源码交付包独立构建脚本
#
# 客户拿到本包后： cmake -B build && cmake --build build -j 即可编译出 demo，无需 SDK 源码。demo
# 只依赖 sdk/include 下的 public API + sdk/lib/libtc_iot_sdk.a（自包含静态库）。
#
# 本包启用了哪些模块由 sdk/module_config.cmake 决定（与 .a 一致）。
cmake_minimum_required(VERSION 3.21)
project(tc_iot_demo C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(PKG_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(SDK_DIR ${PKG_ROOT}/sdk)
set(DEMO_DIR ${PKG_ROOT}/demo)

# 客户包仅 TRTC/PCM 路径，不支持 WebSocket + Opus。
set(AITALK_USE_WEBSOCKET 0)

# 模块默认全开；module_config.cmake 覆盖为本包 .a 实际启用的集合。
set(BUILD_IOT_AV 1)
set(BUILD_IOT_CLOUD_STORAGE 1)
set(BUILD_IOT_AITALK 1)
set(TC_IOT_TRTC_MODE "c")
include(${SDK_DIR}/module_config.cmake OPTIONAL)

set(SDK_A ${SDK_DIR}/lib/libtc_iot_sdk.a)
# c 模式：.a 自包含（含 TRTC-c），仅链系统库。 cc 模式：TRTC 走预编译 libliteav.so/libtxffmpeg.so（C++
# 库不可并入 .a）。
set(TRTC_LINK "")
if(TC_IOT_TRTC_MODE STREQUAL "cc" AND EXISTS ${SDK_DIR}/lib/libliteav.so)
  set(TRTC_LINK ${SDK_DIR}/lib/libliteav.so ${SDK_DIR}/lib/libtxffmpeg.so)
endif()

if(CMAKE_CROSSCOMPILING)
  set(SYS_LIBS m pthread)
  if(TC_IOT_TRTC_MODE STREQUAL "cc")
    list(APPEND SYS_LIBS stdc++)
    if(EXISTS ${SDK_DIR}/lib/libliteav.so)
      if(DEFINED XC_LINK_DIRS AND EXISTS ${XC_LINK_DIRS}/libz.so.1.2.11)
        list(APPEND SYS_LIBS ${XC_LINK_DIRS}/libz.so.1.2.11)
      elseif(DEFINED XC_LINK_DIRS AND EXISTS ${XC_LINK_DIRS}/libz.so.1)
        list(APPEND SYS_LIBS ${XC_LINK_DIRS}/libz.so.1)
      else()
        list(APPEND SYS_LIBS z)
      endif()
    endif()
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "mips")
    list(APPEND SYS_LIBS atomic)
  endif()
else()
  set(SYS_LIBS z m pthread dl)
endif()

# 相对包根目录；运行时须在解压后的包根目录启动 demo（见 README）。
set(_DEMO_MEDIA_DEF "TC_IOT_DEMO_ROOT=\"./demo\"")

# iot_video_demo —— 音视频通话 demo，严格只依赖 public API（include 仅 sdk/include）。
if(BUILD_IOT_AV)
  # minimp4 单头文件库：生成含 #define MINIMP4_IMPLEMENTATION 的 .c 一起编译，否则
  # av_device_linux.c 里的 MP4D_* 在链接期未定义（与 src/demo/iot_video_demo/CMakeLists.txt 一致）。
  set(MINIMP4_C ${CMAKE_CURRENT_BINARY_DIR}/minimp4.c)
  file(WRITE ${MINIMP4_C}
      "#define MINIMP4_IMPLEMENTATION\n"
      "#include \"minimp4.h\"\n")

  add_executable(
    iot_video_demo
    demo/iot_video_demo/main.c
    demo/iot_video_demo/iot_demo.c
    demo/iot_video_demo/cmd_exec.c
    demo/iot_video_demo/file_stream.c
    demo/iot_video_demo/av_device_linux.c
    demo/iot_video_demo/av_sender.c
    demo/iot_video_demo/av_receiver.c
    demo/iot_video_demo/cloud_storage_demo.c
    ${MINIMP4_C})
  target_include_directories(
    iot_video_demo PRIVATE ${DEMO_DIR}/iot_video_demo ${SDK_DIR}/include
                           ${SDK_DIR}/include/hal)
  target_compile_definitions(
    iot_video_demo
    PRIVATE ${_DEMO_MEDIA_DEF} BUILD_IOT_AV=${BUILD_IOT_AV}
            BUILD_IOT_CLOUD_STORAGE=${BUILD_IOT_CLOUD_STORAGE}
            BUILD_IOT_AITALK=${BUILD_IOT_AITALK})
  target_link_libraries(
    iot_video_demo PRIVATE -Wl,--start-group ${SDK_A} -Wl,--end-group
                           ${TRTC_LINK} ${SYS_LIBS})
endif()

# iot_aitalk_demo —— AI 语音 demo（TRTC/PCM，不编译/不链接 Opus）。
if(BUILD_IOT_AITALK)
  add_executable(
    iot_aitalk_demo
    demo/iot_aitalk_demo/main.c demo/iot_aitalk_demo/aitalk_demo.c
    demo/iot_aitalk_demo/audio_device_linux.c demo/iot_aitalk_demo/wave_io.c)
  target_include_directories(iot_aitalk_demo PRIVATE ${DEMO_DIR}/iot_aitalk_demo
                                                     ${SDK_DIR}/include)
  target_compile_definitions(iot_aitalk_demo PRIVATE ${_DEMO_MEDIA_DEF})
  target_link_libraries(
    iot_aitalk_demo PRIVATE -Wl,--start-group ${SDK_A} -Wl,--end-group
                            ${TRTC_LINK} ${SYS_LIBS})
endif()
