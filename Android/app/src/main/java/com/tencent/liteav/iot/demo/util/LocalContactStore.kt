package com.tencent.liteav.iot.demo.util

import android.content.Context
import com.tencent.liteav.iot.TXIoTCallSession
import org.json.JSONArray
import org.json.JSONObject

/**
 * 本地联系人存储：用于保存用户手动添加的 VoIP 用户 / 物联网设备联系人。
 *
 * - VoIP 用户：contact.user_id = modelId/appId/openId
 * - 物联网设备：contact.user_id = productId/deviceName
 */
object LocalContactStore {

    private const val PREF_NAME = "iot_demo_local_contacts"
    private const val KEY_LIST = "list"

    const val TYPE_VOIP = 1
    const val TYPE_IOT = 2

    data class LocalContact(
        val userId: String,
        val userName: String,
        val type: Int
    )

    fun getAll(context: Context): List<LocalContact> {
        val json = context.applicationContext
            .getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
            .getString(KEY_LIST, null) ?: return emptyList()
        return runCatching {
            val arr = JSONArray(json)
            val list = mutableListOf<LocalContact>()
            for (i in 0 until arr.length()) {
                val obj = arr.optJSONObject(i) ?: continue
                val uid = obj.optString("userId")
                if (uid.isNullOrEmpty()) continue
                list.add(
                    LocalContact(
                        userId = uid,
                        userName = obj.optString("userName"),
                        type = obj.optInt("type", TYPE_VOIP)
                    )
                )
            }
            list
        }.getOrDefault(emptyList())
    }

    /**
     * 添加一个本地联系人；若已存在同 userId 则返回 false。
     */
    fun add(context: Context, contact: LocalContact): Boolean {
        val list = getAll(context).toMutableList()
        if (list.any { it.userId == contact.userId }) return false
        list.add(0, contact)
        save(context, list)
        return true
    }

    /**
     * 根据 userId 移除一个本地联系人；若不存在则返回 false。
     */
    fun remove(context: Context, userId: String): Boolean {
        if (userId.isEmpty()) return false
        val list = getAll(context).toMutableList()
        val removed = list.removeAll { it.userId == userId }
        if (removed) save(context, list)
        return removed
    }

    /**
     * 判断某个 userId 是否是本地手动添加的联系人。
     */
    fun contains(context: Context, userId: String): Boolean {
        if (userId.isEmpty()) return false
        return getAll(context).any { it.userId == userId }
    }

    private fun save(context: Context, list: List<LocalContact>) {
        val arr = JSONArray()
        list.forEach { c ->
            arr.put(
                JSONObject()
                    .put("userId", c.userId)
                    .put("userName", c.userName)
                    .put("type", c.type)
            )
        }
        context.applicationContext
            .getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_LIST, arr.toString())
            .apply()
    }

    fun toSessionContact(local: LocalContact): TXIoTCallSession.Contact {
        val c = TXIoTCallSession.Contact()
        c.userId = local.userId
        c.userName = local.userName
        return c
    }
}
