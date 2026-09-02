package com.tencent.liteav.iot.demo

import android.content.Context
import android.content.SharedPreferences

/**
 * 监控相关的用户偏好。
 *
 * 目前只维护一个开关 [autoLaunchPreview]：
 * - `true`（默认）：云端 App 开始拉流（收到 `onMonitorBegin`）时，
 *   自动跳转到 [MonitorActivity] 展示本地预览画面。
 * - `false`：不跳转，直接在后台推流，用户停留在 [HomeActivity]。
 *
 * 该偏好由 [DeviceDetailActivity] 的开关写入，由 [HomeActivity] 的全局
 * `onMonitorBegin` 回调消费。
 */
object MonitorPreferences {

    private const val PREF_NAME = "monitor_prefs"
    private const val KEY_AUTO_LAUNCH_PREVIEW = "auto_launch_preview"

    private lateinit var sp: SharedPreferences

    /** 由 [DemoApplication.onCreate] 调用一次，注入 Application context。 */
    fun init(context: Context) {
        if (!::sp.isInitialized) {
            sp = context.applicationContext.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
        }
    }

    var autoLaunchPreview: Boolean
        get() = sp.getBoolean(KEY_AUTO_LAUNCH_PREVIEW, true)
        set(value) { sp.edit().putBoolean(KEY_AUTO_LAUNCH_PREVIEW, value).apply() }
}
