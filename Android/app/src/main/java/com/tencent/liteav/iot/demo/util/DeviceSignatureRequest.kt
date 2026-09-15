package com.tencent.liteav.iot.demo.util

import android.os.Handler
import android.os.Looper
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

object DeviceSignatureRequest {

    private const val SERVICE = "iotexplorer"
    private const val HOST = "iotexplorer.tencentcloudapi.com"
    private const val ENDPOINT = "https://iotexplorer.tencentcloudapi.com"
    private const val ACTION = "GenSingleDeviceSignatureOfPublic"
    private const val VERSION = "2019-04-23"
    private const val DEFAULT_REGION = "ap-guangzhou"

    private const val CONNECT_TIMEOUT_MS = 10_000
    private const val READ_TIMEOUT_MS = 15_000

    private val executor = Executors.newSingleThreadExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())

    data class DeviceSignatureResult(
        val deviceName: String,
        val deviceSignature: String,
        val requestId: String
    )

    interface Callback {
        fun onSuccess(result: DeviceSignatureResult)
        fun onError(code: String, message: String)
    }

    fun request(
        secretId: String,
        secretKey: String,
        productId: String,
        deviceName: String,
        expire: Int,
        region: String = DEFAULT_REGION,
        callback: Callback
    ) {
        executor.execute {
            try {
                val payload = JSONObject().apply {
                    put("ProductId", productId)
                    put("DeviceName", deviceName)
                    put("Expire", expire)
                }.toString()

                val signed = TencentCloudSigner.sign(
                    secretId = secretId,
                    secretKey = secretKey,
                    service = SERVICE,
                    host = HOST,
                    action = ACTION,
                    payload = payload
                )

                val responseText = post(payload, signed, region)
                val result = parseResponse(responseText)
                postToMain { callback.onSuccess(result) }
            } catch (e: ApiException) {
                postToMain { callback.onError(e.code, e.message ?: "") }
            } catch (e: Exception) {
                postToMain { callback.onError("NetworkError", e.message ?: "请求失败") }
            }
        }
    }

    private fun post(
        payload: String,
        signed: TencentCloudSigner.SignedHeaders,
        region: String
    ): String {
        val connection = (URL(ENDPOINT).openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout = READ_TIMEOUT_MS
            doOutput = true
            setRequestProperty("Content-Type", "application/json; charset=utf-8")
            setRequestProperty("Host", signed.host)
            setRequestProperty("Authorization", signed.authorization)
            setRequestProperty("X-TC-Action", ACTION)
            setRequestProperty("X-TC-Version", VERSION)
            setRequestProperty("X-TC-Timestamp", signed.timestamp.toString())
            setRequestProperty("X-TC-Region", region)
        }

        try {
            connection.outputStream.use { it.write(payload.toByteArray(Charsets.UTF_8)) }

            val stream = if (connection.responseCode in 200..299) {
                connection.inputStream
            } else {
                connection.errorStream ?: connection.inputStream
            }
            return BufferedReader(InputStreamReader(stream, Charsets.UTF_8)).use { it.readText() }
        } finally {
            connection.disconnect()
        }
    }

    private fun parseResponse(responseText: String): DeviceSignatureResult {
        val response = JSONObject(responseText).optJSONObject("Response")
            ?: throw ApiException("InvalidResponse", "响应格式错误：$responseText")

        // 业务错误
        response.optJSONObject("Error")?.let { error ->
            val code = error.optString("Code", "UnknownError")
            val message = error.optString("Message", "接口返回错误")
            throw ApiException(code, message)
        }

        val requestId = response.optString("RequestId", "")
        val signatureInfo = response.optJSONObject("DeviceSignature")
            ?: throw ApiException("InvalidResponse", "缺少 DeviceSignature 字段")

        return DeviceSignatureResult(
            deviceName = signatureInfo.optString("DeviceName", ""),
            deviceSignature = signatureInfo.optString("DeviceSignature", ""),
            requestId = requestId
        )
    }

    private fun postToMain(action: () -> Unit) {
        mainHandler.post(action)
    }

    private class ApiException(val code: String, message: String) : Exception(message)
}
