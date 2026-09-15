package com.tencent.liteav.iot.demo.util

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
