package com.tencent.liteav.iot.demo.util

import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

object TencentCloudSigner {

    data class SignedHeaders(
        val authorization: String,
        val timestamp: Long,
        val host: String
    )

    fun sign(
        secretId: String,
        secretKey: String,
        service: String,
        host: String,
        action: String,
        payload: String,
        timestamp: Long = System.currentTimeMillis() / 1000
    ): SignedHeaders {
        val date = formatUtcDate(timestamp)

        val contentType = "application/json; charset=utf-8"
        val httpRequestMethod = "POST"
        val canonicalUri = "/"
        val canonicalQueryString = ""
        val canonicalHeaders =
            "content-type:${contentType.trim().lowercase(Locale.US)}\n" +
                "host:${host.trim().lowercase(Locale.US)}\n" +
                "x-tc-action:${action.trim().lowercase(Locale.US)}\n"
        val signedHeaders = "content-type;host;x-tc-action"
        val hashedRequestPayload = sha256Hex(payload)
        val canonicalRequest = listOf(
            httpRequestMethod,
            canonicalUri,
            canonicalQueryString,
            canonicalHeaders,
            signedHeaders,
            hashedRequestPayload
        ).joinToString("\n")

        val algorithm = "TC3-HMAC-SHA256"
        val credentialScope = "$date/$service/tc3_request"
        val hashedCanonicalRequest = sha256Hex(canonicalRequest)
        val stringToSign = listOf(
            algorithm,
            timestamp.toString(),
            credentialScope,
            hashedCanonicalRequest
        ).joinToString("\n")

        val secretDate = hmacSha256("TC3$secretKey".toByteArray(Charsets.UTF_8), date)
        val secretService = hmacSha256(secretDate, service)
        val secretSigning = hmacSha256(secretService, "tc3_request")
        val signature = toHex(hmacSha256(secretSigning, stringToSign))

        val authorization =
            "$algorithm " +
                "Credential=$secretId/$credentialScope, " +
                "SignedHeaders=$signedHeaders, " +
                "Signature=$signature"

        return SignedHeaders(authorization = authorization, timestamp = timestamp, host = host)
    }

    private fun formatUtcDate(timestamp: Long): String {
        val sdf = SimpleDateFormat("yyyy-MM-dd", Locale.US)
        sdf.timeZone = TimeZone.getTimeZone("UTC")
        return sdf.format(Date(timestamp * 1000))
    }

    private fun sha256Hex(s: String): String {
        val md = MessageDigest.getInstance("SHA-256")
        return toHex(md.digest(s.toByteArray(Charsets.UTF_8)))
    }

    private fun hmacSha256(key: ByteArray, msg: String): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(msg.toByteArray(Charsets.UTF_8))
    }

    private fun toHex(bytes: ByteArray): String {
        val sb = StringBuilder(bytes.size * 2)
        for (b in bytes) {
            sb.append(String.format("%02x", b))
        }
        return sb.toString()
    }
}
