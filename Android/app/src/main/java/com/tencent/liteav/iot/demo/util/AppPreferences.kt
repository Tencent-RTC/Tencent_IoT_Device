package com.tencent.liteav.iot.demo.util

import android.content.Context
import android.content.SharedPreferences

object AppPreferences {

    const val FLAVOR_RELEASE = "RELEASE"
    const val FLAVOR_TRIAL = "DEMO"
    const val FLAVOR_DEBUG = "DEBUG"

    private const val PREF_NAME = "app_prefs"
    private const val KEY_PRODUCT_ID = "product_id"
    private const val KEY_DEVICE_ID = "device_id"
    private const val KEY_REGION = "region"
    private const val KEY_AUTO_LAUNCH_PREVIEW = "auto_launch_preview"
    private const val KEY_MINI_PROGRAM_FLAVOR = "mini_program_flavor"
    private const val KEY_SECRET_ID = "cloud_secret_id"
    private const val KEY_HOME_BIND_TIP_DISMISSED = "home_bind_tip_dismissed"

    private lateinit var sp: SharedPreferences

    fun init(context: Context) {
        if (!::sp.isInitialized) {
            sp = context.applicationContext.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
        }
        SecureStore.init(context)
    }

    val productId: String get() = sp.getString(KEY_PRODUCT_ID, "").orEmpty()
    val deviceId: String get() = sp.getString(KEY_DEVICE_ID, "").orEmpty()
    val deviceSecret: String get() = SecureStore.deviceSecret
    val region: String get() = sp.getString(KEY_REGION, "").orEmpty()

    val isLoggedIn: Boolean
        get() = productId.isNotEmpty() && deviceId.isNotEmpty() && deviceSecret.isNotEmpty()

    fun saveLogin(productId: String, deviceId: String, deviceSecret: String, region: String) {
        sp.edit()
            .putString(KEY_PRODUCT_ID, productId)
            .putString(KEY_DEVICE_ID, deviceId)
            .putString(KEY_REGION, region)
            .apply()
        SecureStore.deviceSecret = deviceSecret
    }

    fun clearLogin() {
        sp.edit()
            .remove(KEY_PRODUCT_ID)
            .remove(KEY_DEVICE_ID)
            .remove(KEY_REGION)
            .apply()
        SecureStore.clearDeviceSecret()
    }

    var autoLaunchPreview: Boolean
        get() = sp.getBoolean(KEY_AUTO_LAUNCH_PREVIEW, true)
        set(value) {
            sp.edit().putBoolean(KEY_AUTO_LAUNCH_PREVIEW, value).apply()
        }

    var miniProgramFlavor: String
        get() = sp.getString(KEY_MINI_PROGRAM_FLAVOR, FLAVOR_RELEASE) ?: FLAVOR_RELEASE
        set(value) {
            sp.edit().putString(KEY_MINI_PROGRAM_FLAVOR, value).apply()
        }

    var secretId: String
        get() = sp.getString(KEY_SECRET_ID, "").orEmpty()
        set(value) {
            sp.edit().putString(KEY_SECRET_ID, value).apply()
        }

    var secretKey: String
        get() = SecureStore.secretKey
        set(value) {
            SecureStore.secretKey = value
        }

    var homeBindTipDismissed: Boolean
        get() = sp.getBoolean(KEY_HOME_BIND_TIP_DISMISSED, false)
        set(value) {
            sp.edit().putBoolean(KEY_HOME_BIND_TIP_DISMISSED, value).apply()
        }
}