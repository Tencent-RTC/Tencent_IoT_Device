package com.tencent.liteav.iot.demo

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.updatePadding
import java.io.File
import java.io.FileOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import com.tencent.liteav.iot.demo.util.FileLogWriter

class DeviceLogActivity : AppCompatActivity() {

    private lateinit var llHardwareInfo: LinearLayout
    private lateinit var btnShareLog: View
    private lateinit var tvHeroModel: TextView
    private lateinit var tvHeroBrand: TextView
    private lateinit var tvLogFilesDesc: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setupImmersiveStatusBar()
        setContentView(R.layout.activity_device_log)

        llHardwareInfo = findViewById(R.id.ll_hardware_info)
        btnShareLog = findViewById(R.id.btn_share_log)
        tvHeroModel = findViewById(R.id.tv_hero_model)
        tvHeroBrand = findViewById(R.id.tv_hero_brand)
        tvLogFilesDesc = findViewById(R.id.tv_log_files_desc)

        findViewById<ImageView>(R.id.iv_log_back).setOnClickListener { finish() }
        btnShareLog.setOnClickListener { shareLogs() }

        applyWindowInsets()
        renderHero()
        renderHardwareInfo()
    }

    private fun setupImmersiveStatusBar() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = Color.TRANSPARENT
        // 页面底色为浅色 (#F5F7FB)，状态栏图标需为深色
        WindowInsetsControllerCompat(window, window.decorView)
            .isAppearanceLightStatusBars = true
    }

    private fun applyWindowInsets() {
        val header = findViewById<LinearLayout>(R.id.ll_log_header)
        val bottomBar = findViewById<FrameLayout>(R.id.fl_log_bottom_bar)
        val headerBaseTop = header.paddingTop
        val bottomBaseBottom = bottomBar.paddingBottom
        ViewCompat.setOnApplyWindowInsetsListener(header) { v, insets ->
            val top = insets.getInsets(WindowInsetsCompat.Type.statusBars()).top
            v.updatePadding(top = headerBaseTop + top)
            insets
        }
        ViewCompat.setOnApplyWindowInsetsListener(bottomBar) { v, insets ->
            val bottom = insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom
            v.updatePadding(bottom = bottomBaseBottom + bottom)
            insets
        }
    }

    override fun onResume() {
        super.onResume()
        refreshLogFilesSummary()
    }

    private fun renderHero() {
        tvHeroModel.text = Build.MODEL.ifBlank { "--" }
        tvHeroBrand.text = Build.BRAND.ifBlank { "--" }
    }

    private fun renderHardwareInfo() {
        val totalMemMb = totalMemoryMb()
        val abis = Build.SUPPORTED_ABIS.joinToString(", ")

        val rows = listOf(
            Row(R.drawable.ic_log_brand, "品牌", Build.BRAND),
            Row(R.drawable.ic_log_phone, "型号", Build.MODEL),
            Row(R.drawable.ic_log_info, "设备名称", Build.DEVICE),
            Row(R.drawable.ic_log_info, "制造商", Build.MANUFACTURER),
            Row(R.drawable.ic_log_chip, "硬件", Build.HARDWARE),
            Row(R.drawable.ic_log_chip, "CPU ABI", abis),
            Row(
                R.drawable.ic_log_android,
                "Android 版本",
                "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})"
            ),
            Row(R.drawable.ic_log_memory, "内存", if (totalMemMb > 0) "${totalMemMb} MB" else "--"),
            Row(
                R.drawable.ic_log_screen,
                "屏幕分辨率",
                "${resources.displayMetrics.widthPixels} x ${resources.displayMetrics.heightPixels}"
            )
        )

        llHardwareInfo.removeAllViews()
        rows.forEachIndexed { index, row ->
            addInfoRow(row.icon, row.label, row.value)
            if (index != rows.lastIndex) {
                addDivider()
            }
        }
    }

    private data class Row(val icon: Int, val label: String, val value: String)

    private fun addInfoRow(iconRes: Int, label: String, value: String) {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            setPadding(0, dp(12), 0, dp(12))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
        }

        // 左侧图标底框
        val iconBox = android.widget.FrameLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(36), dp(36))
            setBackgroundResource(R.drawable.bg_log_row_icon)
        }
        val iconView = ImageView(this).apply {
            layoutParams = android.widget.FrameLayout.LayoutParams(
                dp(20), dp(20), android.view.Gravity.CENTER
            )
            setImageResource(iconRes)
        }
        iconBox.addView(iconView)

        val labelView = TextView(this).apply {
            text = label
            textSize = 14f
            setTextColor(Color.parseColor("#4E5969"))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                marginStart = dp(12)
            }
        }
        val valueView = TextView(this).apply {
            text = value.ifBlank { "--" }
            textSize = 14f
            setTextColor(Color.parseColor("#1D2129"))
            gravity = android.view.Gravity.END
            maxLines = 2
            ellipsize = android.text.TextUtils.TruncateAt.END
            layoutParams = LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f
            ).apply {
                marginStart = dp(12)
            }
        }
        row.addView(iconBox)
        row.addView(labelView)
        row.addView(valueView)
        llHardwareInfo.addView(row)
    }

    private fun addDivider() {
        val divider = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 1
            ).apply {
                marginStart = dp(48)
            }
            setBackgroundColor(Color.parseColor("#F0F1F5"))
        }
        llHardwareInfo.addView(divider)
    }

    private fun refreshLogFilesSummary() {
        val logDir = FileLogWriter.getLogDir()
        val logFiles = logDir?.listFiles { f -> f.isFile && f.name.endsWith(".log") }
        if (logFiles.isNullOrEmpty()) {
            tvLogFilesDesc.text = getString(R.string.log_page_log_files_empty_desc)
            return
        }
        val totalBytes = logFiles.sumOf { it.length() }
        tvLogFilesDesc.text = getString(
            R.string.log_page_log_files_desc,
            logFiles.size,
            formatSize(totalBytes)
        )
    }

    private fun formatSize(bytes: Long): String {
        return when {
            bytes < 1024 -> "${bytes} B"
            bytes < 1024 * 1024 -> String.format("%.1f KB", bytes / 1024.0)
            else -> String.format("%.2f MB", bytes / (1024.0 * 1024.0))
        }
    }

    private fun totalMemoryMb(): Long {
        return try {
            val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val mi = ActivityManager.MemoryInfo()
            am.getMemoryInfo(mi)
            mi.totalMem / (1024 * 1024)
        } catch (e: Exception) {
            0L
        }
    }

    private fun shareLogs() {
        val logDir = FileLogWriter.getLogDir()
        val logFiles = logDir?.listFiles { f -> f.isFile && f.name.endsWith(".log") }
        if (logFiles.isNullOrEmpty()) {
            toast(getString(R.string.log_page_no_log))
            return
        }

        btnShareLog.isEnabled = false
        toast(getString(R.string.log_page_sharing))

        val zipFile = File(cacheDir, "iot_logs.zip")
        try {
            zipFiles(logFiles, zipFile)
            val uri = FileProvider.getUriForFile(this, "$packageName.fileprovider", zipFile)
            val intent = Intent(Intent.ACTION_SEND).apply {
                type = "application/zip"
                putExtra(Intent.EXTRA_STREAM, uri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            startActivity(Intent.createChooser(intent, getString(R.string.log_page_share_title)))
        } catch (e: Exception) {
            toast(getString(R.string.log_page_share_failed, e.message ?: ""))
        } finally {
            btnShareLog.isEnabled = true
        }
    }

    private fun zipFiles(files: Array<File>, zipFile: File) {
        FileOutputStream(zipFile).use { fos ->
            ZipOutputStream(fos).use { zos ->
                files.forEach { file ->
                    zos.putNextEntry(ZipEntry(file.name))
                    file.inputStream().use { input ->
                        input.copyTo(zos)
                    }
                    zos.closeEntry()
                }
            }
        }
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}
