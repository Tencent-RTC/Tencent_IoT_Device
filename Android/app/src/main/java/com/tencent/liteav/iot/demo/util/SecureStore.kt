package com.tencent.liteav.iot.demo.util

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKeys

object SecureStore {

    private const val TAG = "SecureStore"
    private const val FILE_NAME = "secure_prefs"

    private const val KEY_DEVICE_SECRET = "device_secret"
    private const val KEY_CLOUD_SECRET_KEY = "cloud_secret_key"

    private lateinit var sp: SharedPreferences

    fun init(context: Context) {
        if (::sp.isInitialized) return
        val appContext = context.applicationContext
        sp = try {
            createEncryptedPrefs(appContext)
        } catch (e: Exception) {
            Log.w(TAG, "create EncryptedSharedPreferences failed, reset and retry", e)
            clearLegacyPrefsFile(appContext)
            createEncryptedPrefs(appContext)
        }
    }

    /** 清除加密 SP 底层文件，用于初始化失败后的兜底重建。 */
    private fun clearLegacyPrefsFile(context: Context) {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
            context.deleteSharedPreferences(FILE_NAME)
        } else {
            context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
                .edit().clear().commit()
        }
    }

    private fun createEncryptedPrefs(context: Context): SharedPreferences {
        val masterKeyAlias = MasterKeys.getOrCreate(MasterKeys.AES256_GCM_SPEC)
        return EncryptedSharedPreferences.create(
            FILE_NAME,
            masterKeyAlias,
            context,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

    var deviceSecret: String
        get() = sp.getString(KEY_DEVICE_SECRET, "").orEmpty()
        set(value) { sp.edit().putString(KEY_DEVICE_SECRET, value).apply() }

    var secretKey: String
        get() = sp.getString(KEY_CLOUD_SECRET_KEY, "").orEmpty()
        set(value) { sp.edit().putString(KEY_CLOUD_SECRET_KEY, value).apply() }

    fun clearDeviceSecret() {
        sp.edit().remove(KEY_DEVICE_SECRET).apply()
    }

    fun clearSecretKey() {
        sp.edit().remove(KEY_CLOUD_SECRET_KEY).apply()
    }
}
