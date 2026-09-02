package com.tencent.liteav.iot.demo

/**
 * 进程内会话信息缓存：保存当前登录成功的设备信息，
 * 供物模型、监控、云存等页面回显。
 */
object IoTSessionStore {
    @Volatile
    var productId: String = ""
        private set

    @Volatile
    var deviceId: String = ""
        private set

    @Volatile
    var region: String = ""
        private set

    fun update(productId: String, deviceId: String, region: String) {
        this.productId = productId
        this.deviceId = deviceId
        this.region = region
    }

    fun clear() {
        productId = ""
        deviceId = ""
        region = ""
    }
}
