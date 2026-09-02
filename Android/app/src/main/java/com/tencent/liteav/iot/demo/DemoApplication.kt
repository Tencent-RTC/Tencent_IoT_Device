package com.tencent.liteav.iot.demo

import android.app.Application
import android.util.Log
import com.tencent.liteav.iot.TXIoTDeviceEngine
import com.tencent.liteav.iot.TXIoTError

class DemoApplication : Application() {

    companion object Companion {
        private const val TAG = "DemoApp"
    }

    val engineListener: TXIoTDeviceEngine.Listener = object : TXIoTDeviceEngine.Listener {
        override fun onLog(level: TXIoTDeviceEngine.LogLevel, log: String) {
            Log.d(TAG, "[SDK][$level] $log")
        }

        override fun onDeviceStateChanged(
            oldState: TXIoTDeviceEngine.DeviceState,
            newState: TXIoTDeviceEngine.DeviceState
        ) {
            Log.d(TAG, "[SDK] deviceState: $oldState -> $newState")
            DeviceStateObservable.update(newState)
        }
    }

    private val engine: TXIoTDeviceEngine by lazy {
        TXIoTDeviceEngine.getInstance(applicationContext)
    }

    override fun onCreate() {
        super.onCreate()
        MonitorPreferences.init(this)
        initIoTEngine()
    }

    private fun initIoTEngine() {
        val config = TXIoTDeviceEngine.Config().apply {
            storagePath = filesDir.absolutePath
            logLevel = TXIoTDeviceEngine.LogLevel.INFO
        }
        engine.addListener(engineListener)
        val ret = engine.initSDK(config)
        if (ret != TXIoTError.SUCCESS && ret != TXIoTError.ALREADY_INITIALIZED) {
            Log.w(TAG, "TXIoTDeviceEngine.initSDK failed, code=$ret")
        }
    }
}
