package com.tencent.liteav.iot.demo

import android.app.Application
import android.util.Log
import com.tencent.liteav.iot.TXIoTDeviceEngine
import com.tencent.liteav.iot.TXIoTError
import com.tencent.liteav.iot.demo.util.AppPreferences
import com.tencent.liteav.iot.demo.util.FileLogWriter

class DemoApplication : Application() {

    companion object Companion {
        private const val TAG = "DemoApp"
        private val PUBLIC_REGIONS = setOf("china", "us-east", "europe", "ap-bangkok")

        fun publicRegion(region: String): String =
            if (region in PUBLIC_REGIONS) region else "china"
    }

    val engineListener: TXIoTDeviceEngine.Listener = object : TXIoTDeviceEngine.Listener {
        override fun onLog(level: TXIoTDeviceEngine.LogLevel, log: String) {
            Log.d(TAG, "[SDK][$level] $log")
            FileLogWriter.write(level.name, log)
        }

        override fun onDeviceStateChanged(
            oldState: TXIoTDeviceEngine.DeviceState,
            newState: TXIoTDeviceEngine.DeviceState
        ) {
            Log.d(TAG, "[SDK] deviceState: $oldState -> $newState")
            FileLogWriter.write("STATE", "$oldState -> $newState")
            DeviceStateObservable.update(newState)
        }
    }

    private val engine: TXIoTDeviceEngine by lazy {
        TXIoTDeviceEngine.getInstance(applicationContext)
    }

    private var initedRegion: String? = null

    override fun onCreate() {
        super.onCreate()
        AppPreferences.init(this)
        FileLogWriter.init(filesDir)
        val ret = ensureRegion(publicRegion(AppPreferences.region))
        if (ret != TXIoTError.SUCCESS && ret != TXIoTError.ALREADY_INITIALIZED) {
            Log.w(TAG, "TXIoTDeviceEngine.initSDK failed, code=$ret")
        }
        engine.callExperimentApi(
            "set_wx_miniapp_flavor",
            AppPreferences.miniProgramFlavor,
            null
        )
    }

    fun ensureRegion(region: String): Int {
        val target = publicRegion(region)
        if (initedRegion == target) {
            return TXIoTError.SUCCESS
        }
        if (initedRegion != null) {
            engine.uninitSDK()
            initedRegion = null
        }
        val config = TXIoTDeviceEngine.Config().apply {
            storagePath = filesDir.absolutePath
            logLevel = TXIoTDeviceEngine.LogLevel.INFO
            this.region = target
        }
        engine.addListener(engineListener)
        val ret = engine.initSDK(config)
        if (ret == TXIoTError.SUCCESS || ret == TXIoTError.ALREADY_INITIALIZED) {
            initedRegion = target
        }
        return ret
    }
}
