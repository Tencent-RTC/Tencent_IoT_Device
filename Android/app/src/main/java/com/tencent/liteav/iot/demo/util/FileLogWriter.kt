package com.tencent.liteav.iot.demo.util

import android.util.Log
import java.io.File
import java.io.FileWriter
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object FileLogWriter {

    private const val TAG = "FileLogWriter"
    private const val MAX_FILE_SIZE = 5L * 1024 * 1024
    private const val MAX_BACKUP_FILES = 3

    private val lock = Any()
    private val timeFormat = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)
    private var logDir: File? = null

    fun init(dir: File) {
        synchronized(lock) {
            logDir = File(dir, "logs").apply { mkdirs() }
        }
    }

    fun getLogDir(): File? = logDir

    fun write(level: String, message: String) {
        synchronized(lock) {
            val dir = logDir ?: return
            val file = File(dir, "iot.log")
            try {
                if (file.exists() && file.length() >= MAX_FILE_SIZE) {
                    rotate(file)
                }
                FileWriter(file, true).use { writer ->
                    writer.append(timeFormat.format(Date()))
                        .append(" [").append(level).append("] ")
                        .append(message)
                        .append('\n')
                }
            } catch (e: IOException) {
                Log.w(TAG, "write log failed: ${e.message}")
            }
        }
    }

    private fun rotate(file: File) {
        for (i in MAX_BACKUP_FILES - 1 downTo 1) {
            val src = File(file.parentFile, "${file.name}.$i")
            val dst = File(file.parentFile, "${file.name}.${i + 1}")
            if (src.exists()) {
                src.renameTo(dst)
            }
        }
        val backup = File(file.parentFile, "${file.name}.1")
        file.renameTo(backup)
    }
}
