# 本交付包 libtc_iot_sdk.a 实际启用的模块（由 build.sh 打包生成，勿手改）。
# 独立构建读取它设同名宏，使全量 demo 源码编译与 .a 对齐，避免调用被裁模块。
set(BUILD_IOT_AV 1)
set(BUILD_IOT_CLOUD_STORAGE 1)
set(BUILD_IOT_AITALK 1)
set(BUILD_OPUS 0)
set(TC_IOT_TRTC_MODE "cc")
