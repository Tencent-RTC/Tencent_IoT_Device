if(ESP_PLATFORM)
  return()
endif()

set(TARGET_APP_NAME iot_aitalk_demo)
set(ROOT_DIR ${CMAKE_SOURCE_DIR})

include(${ROOT_DIR}/config.cmake)

# 按平台选择音频设备实现 BK7258 固件不走本 CMake：platform CMake 按 IOT_BOARD 选
# audio_device_beken_{amp,smp}_{ai,av}.c。
if(ESP_PLATFORM)
  set(AUDIO_DEVICE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/audio_device_esp32.c)
elseif(BEKEN_PLATFORM)
  message(
    FATAL_ERROR
      "iot_aitalk_demo host CMakeLists does not build Beken device sources; "
      "use platform/bk7258_{smp,amp} CMake (IOT_BOARD=ai|av).")
elseif(AITALK_DEMO_PLATFORM STREQUAL "mac")
  set(AUDIO_DEVICE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/audio_device_mac.mm)
else()
  set(AUDIO_DEVICE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/audio_device_linux.c
                       ${CMAKE_CURRENT_SOURCE_DIR}/wave_io.c)
endif()

set(IOT_AITALK_SRC_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/aitalk_demo.c ${AUDIO_DEVICE_SRC})

if(NOT TC_IOT_SDK_LIB_PATH)
  message(STATUS "Demo: TC_IOT_SDK_LIB_PATH not set, skip iot_aitalk_demo")
  return()
endif()

add_executable(${TARGET_APP_NAME} ${IOT_AITALK_SRC_LIST})

target_include_directories(
  ${TARGET_APP_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                             ${ROOT_DIR}/src/api/iot ${ROOT_DIR}/src/base/hal)

if(AITALK_USE_WEBSOCKET)
  target_include_directories(
    ${TARGET_APP_NAME} PRIVATE ${ROOT_DIR}/src/third_party/opus/src/include
                               ${ROOT_DIR}/src/third_party/opus)
endif()

target_compile_definitions(
  ${TARGET_APP_NAME}
  PRIVATE HAVE_STDINT_H
          HTTP_DO_NOT_USE_CUSTOM_CONFIG
          MQTT_DO_NOT_USE_CUSTOM_CONFIG
          FILE_SYSTEM_DISABLED=${FILE_SYSTEM_DISABLED}
          KVSTORE_IO_INTERVAL=${KVSTORE_IO_INTERVAL}
          PRINT_PACKET_HEX_BUFFER=${PRINT_PACKET_HEX_BUFFER}
          PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

if(WIN32)
  target_link_libraries(${TARGET_APP_NAME} PRIVATE ${TC_IOT_SDK_LIB_PATH}
                                                   Ws2_32.lib)
elseif(APPLE)
  target_link_libraries(${TARGET_APP_NAME} PRIVATE ${TC_IOT_SDK_LIB_PATH} z m
                                                   pthread)
  if(BUILD_OPUS)
    target_link_libraries(${TARGET_APP_NAME} PRIVATE libopus)
  endif()
  find_library(FOUNDATION_FRAMEWORK Foundation)
  target_link_libraries(${TARGET_APP_NAME} PRIVATE ${FOUNDATION_FRAMEWORK})
  find_library(SYSTEM_CONFIGURATION_FRAMEWORK SystemConfiguration)
  target_link_libraries(${TARGET_APP_NAME}
                        PRIVATE ${SYSTEM_CONFIGURATION_FRAMEWORK})
  if(NOT IOS)
    find_library(APPKIT_FRAMEWORK AppKit)
    target_link_libraries(${TARGET_APP_NAME} PRIVATE ${APPKIT_FRAMEWORK})
  endif()
  # audio_device_mac.mm：AVFoundation 权限/设备 + AudioToolbox AudioUnit I/O。
  if(AITALK_DEMO_PLATFORM STREQUAL "mac")
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
    target_link_libraries(
      ${TARGET_APP_NAME} PRIVATE ${AVFOUNDATION_FRAMEWORK}
                                 ${AUDIOTOOLBOX_FRAMEWORK} ${COREAUDIO_FRAMEWORK})
  endif()
else()
  set(_demo_syslibs m pthread)
  if(NOT CMAKE_CROSSCOMPILING)
    list(APPEND _demo_syslibs z dl)
  endif()
  if(USE_TRTC_C AND ${BUILD_TRTC} EQUAL 1)
    target_link_libraries(
      ${TARGET_APP_NAME} PRIVATE -Wl,--start-group ${TC_IOT_SDK_LIB_PATH}
                                 -Wl,--end-group ${_demo_syslibs})
  elseif(CMAKE_CROSSCOMPILING)
    if(TRTC_CC_LIB_ARCH)
      set(_trtc_lib_dir
          ${ROOT_DIR}/src/third_party/iot_trtc/lib/${TRTC_CC_LIB_ARCH})
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
      target_link_libraries(
        ${TARGET_APP_NAME}
        PRIVATE -Wl,--start-group
                ${TC_IOT_SDK_LIB_PATH}
                -Wl,--end-group
                ${_liteav_so}
                ${_txffmpeg_so}
                ${_cc_extra}
                ${_zlib_link}
                ${_demo_syslibs})
    else()
      set(_cc_extra stdc++)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "mips")
        list(APPEND _cc_extra atomic)
      endif()
      target_link_libraries(
        ${TARGET_APP_NAME}
        PRIVATE -Wl,--start-group ${TC_IOT_SDK_LIB_PATH} -Wl,--end-group
                ${_cc_extra} ${_demo_syslibs})
    endif()
  else()
    find_library(
      LIBLITEAV libliteav.so ${ROOT_DIR}/src/third_party/iot_trtc/lib/x64/
      NO_DEFAULT_PATH)
    find_library(
      LIBTXFFMPEG libtxffmpeg.so
      ${ROOT_DIR}/src/third_party/iot_trtc/lib/x64/ NO_DEFAULT_PATH)
    target_link_libraries(
      ${TARGET_APP_NAME}
      PRIVATE -Wl,--start-group ${TC_IOT_SDK_LIB_PATH} -Wl,--end-group
              ${LIBLITEAV} ${LIBTXFFMPEG} ${_demo_syslibs})
  endif()
  if(AITALK_USE_WEBSOCKET AND BUILD_OPUS)
    target_link_libraries(${TARGET_APP_NAME} PRIVATE libopus)
  endif()
endif()
