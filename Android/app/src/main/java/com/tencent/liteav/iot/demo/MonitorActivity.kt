package com.tencent.liteav.iot.demo

import android.Manifest
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.WindowManager
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.constraintlayout.widget.ConstraintLayout
import androidx.constraintlayout.widget.ConstraintSet
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.updatePadding
import com.tencent.liteav.iot.TXIoTError
import com.tencent.liteav.iot.TXIoTMonitorSession
import com.tencent.rtmp.ui.TXCloudVideoView

class MonitorActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "MonitorActivity"
        private const val REQ_AV_PERM = 2001

        const val EXTRA_QUALITY = "extra_quality"

        const val EXTRA_CUSTOM_DATA = "extra_custom_data"
    }

    private lateinit var tvStatus: TextView
    private lateinit var tvHint: TextView
    private lateinit var tvQuality: TextView
    private lateinit var btnMic: View
    private lateinit var videoView: TXCloudVideoView
    private lateinit var ivFullscreen: ImageView
    private lateinit var clHeader: View
    private lateinit var llActions: View
    private lateinit var clVideo: ConstraintLayout
    private lateinit var ivMicIcon: ImageView
    private lateinit var tvMicLabel: TextView
    private lateinit var tvRatio: TextView

    private val ratioOptions = listOf("16:9", "4:3", "1:1", "3:4", "9:16")
    private var ratioIndex = 0

    private var isCameraOpened = false

    private var isStreaming = true

    private var isTalking = false

    private var currentQuality: TXIoTMonitorSession.VideoQuality =
        TXIoTMonitorSession.VideoQuality.HD

    private val monitorSession: TXIoTMonitorSession by lazy { TXIoTMonitorSession.getInstance() }

    private val monitorListener = object : TXIoTMonitorSession.Listener {
        override fun onMonitorBegin(
            option: TXIoTMonitorSession.Option?,
            customData: String?
        ) {
            option?.videoQuality?.let { currentQuality = it }
            isStreaming = true
            openCameraIfNeeded()
            openMicrophoneIfNeeded()
            tvStatus.text = getString(R.string.monitor_status_online)
            tvQuality.text = qualityLabel(currentQuality)
            tvHint.visibility = View.GONE
            btnMic.isEnabled = true
            refreshMicUi()
            toast("对端已连接")
        }

        override fun onMonitorEnd() {
            isStreaming = false
            if (!isFinishing) {
                finish()
            }
        }

        override fun onMonitorSwitch(quality: TXIoTMonitorSession.VideoQuality?) {
            quality ?: return
            currentQuality = quality
            tvQuality.text = qualityLabel(quality)
        }

        override fun onPTZCommandReceived(
            ptzCommand: TXIoTMonitorSession.PTZCommand?,
            speed: Int
        ) {
            val label = when (ptzCommand) {
                TXIoTMonitorSession.PTZCommand.UP -> "上"
                TXIoTMonitorSession.PTZCommand.DOWN -> "下"
                TXIoTMonitorSession.PTZCommand.LEFT -> "左"
                TXIoTMonitorSession.PTZCommand.RIGHT -> "右"
                TXIoTMonitorSession.PTZCommand.ZOOM_IN -> "放大"
                TXIoTMonitorSession.PTZCommand.ZOOM_OUT -> "缩小"
                TXIoTMonitorSession.PTZCommand.STOP -> "停止"
                null -> "未知"
            }
            toast("云台指令：$label speed=$speed")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_monitor)
        setupImmersiveStatusBar()

        parseIntent()

        tvStatus = findViewById(R.id.tv_monitor_status)
        tvHint = findViewById(R.id.tv_monitor_hint)
        tvQuality = findViewById(R.id.tv_monitor_quality)
        videoView = findViewById(R.id.video_monitor_preview)
        btnMic = findViewById(R.id.btn_monitor_mic)
        ivFullscreen = findViewById(R.id.iv_monitor_fullscreen)
        clHeader = findViewById(R.id.cl_monitor_header)
        llActions = findViewById(R.id.ll_monitor_actions)
        clVideo = findViewById(R.id.cl_monitor_video)
        ivMicIcon = findViewById(R.id.iv_monitor_mic_icon)
        tvMicLabel = findViewById(R.id.tv_monitor_mic_label)
        tvRatio = findViewById(R.id.tv_monitor_ratio)

        val headerBasePaddingTop = clHeader.paddingTop
        ViewCompat.setOnApplyWindowInsetsListener(clHeader) { view, insets ->
            val statusBarTop = insets.getInsets(WindowInsetsCompat.Type.statusBars()).top
            view.updatePadding(top = headerBasePaddingTop + statusBarTop)
            insets
        }

        btnMic.setOnClickListener { toggleTalk() }
        tvQuality.setOnClickListener { toggleQuality() }
        tvQuality.text = qualityLabel(currentQuality)
        ivFullscreen.setOnClickListener { toggleFullscreen() }
        tvRatio.setOnClickListener { toggleRatio() }
        tvRatio.text = ratioOptions[ratioIndex]
        refreshMicUi()

        monitorSession.addListener(monitorListener)

        tvStatus.text = getString(R.string.monitor_status_online)
        tvHint.visibility = View.GONE

        if (ensureAvPermission()) {
            enterMonitor()
        }

        applyOrientationUi(resources.configuration.orientation)
    }

    private fun setupImmersiveStatusBar() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = Color.TRANSPARENT
        WindowInsetsControllerCompat(window, window.decorView)
            .isAppearanceLightStatusBars = false
    }

    private fun toggleFullscreen() {
        requestedOrientation = if (resources.configuration.orientation ==
            Configuration.ORIENTATION_LANDSCAPE
        ) {
            ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        } else {
            ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        applyOrientationUi(newConfig.orientation)
    }

    private fun applyOrientationUi(orientation: Int) {
        val landscape = orientation == Configuration.ORIENTATION_LANDSCAPE
        clHeader.visibility = if (landscape) View.GONE else View.VISIBLE
        llActions.visibility = if (landscape) View.GONE else View.VISIBLE
        tvRatio.visibility = if (landscape) View.GONE else View.VISIBLE
        ivFullscreen.setImageResource(
            if (landscape) R.drawable.ic_monitor_shrink else R.drawable.ic_monitor_fullscreen
        )
        if (landscape) {
            window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
        }
        val set = ConstraintSet().apply { clone(clVideo.parent as ConstraintLayout) }
        val id = clVideo.id
        if (landscape) {
            set.setDimensionRatio(id, null)
            set.connect(id, ConstraintSet.TOP, ConstraintSet.PARENT_ID, ConstraintSet.TOP)
            set.connect(id, ConstraintSet.BOTTOM, ConstraintSet.PARENT_ID, ConstraintSet.BOTTOM)
            set.connect(id, ConstraintSet.START, ConstraintSet.PARENT_ID, ConstraintSet.START)
            set.connect(id, ConstraintSet.END, ConstraintSet.PARENT_ID, ConstraintSet.END)
            set.setMargin(id, ConstraintSet.START, 0)
            set.setMargin(id, ConstraintSet.END, 0)
            set.setMargin(id, ConstraintSet.TOP, 0)
        } else {
            set.setDimensionRatio(id, "H,${ratioOptions[ratioIndex]}")
            set.connect(id, ConstraintSet.TOP, R.id.cl_monitor_header, ConstraintSet.BOTTOM)
            set.connect(id, ConstraintSet.BOTTOM, R.id.ll_monitor_actions, ConstraintSet.TOP)
            set.connect(id, ConstraintSet.START, ConstraintSet.PARENT_ID, ConstraintSet.START)
            set.connect(id, ConstraintSet.END, ConstraintSet.PARENT_ID, ConstraintSet.END)
            val h = (16 * resources.displayMetrics.density).toInt()
            set.setMargin(id, ConstraintSet.START, h)
            set.setMargin(id, ConstraintSet.END, h)
            set.setMargin(id, ConstraintSet.TOP, (8 * resources.displayMetrics.density).toInt())
        }
        set.applyTo(clVideo.parent as ConstraintLayout)
    }

    private fun parseIntent() {
        val qName = intent?.getStringExtra(EXTRA_QUALITY).orEmpty()
        if (qName.isNotEmpty()) {
            currentQuality = runCatching {
                TXIoTMonitorSession.VideoQuality.valueOf(qName)
            }.getOrDefault(TXIoTMonitorSession.VideoQuality.HD)
        }
    }

    override fun onDestroy() {
        stopMonitor()
        monitorSession.removeListener(monitorListener)
        super.onDestroy()
    }

    private fun ensureAvPermission(): Boolean {
        val perms = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        val missing = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) return true
        ActivityCompat.requestPermissions(this, missing.toTypedArray(), REQ_AV_PERM)
        return false
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_AV_PERM) return
        val allGranted = grantResults.isNotEmpty() &&
                grantResults.all { it == PackageManager.PERMISSION_GRANTED }
        if (allGranted) {
            enterMonitor()
        } else {
            toast("需要相机和麦克风权限才能开启监控")
        }
    }

    private fun enterMonitor() {
        openCameraIfNeeded()
        openMicrophoneIfNeeded()
        refreshMicUi()
    }

    private fun openCameraIfNeeded() {
        if (isCameraOpened) return
        val option = TXIoTMonitorSession.Option().apply {
            videoQuality = currentQuality
        }
        val ret = monitorSession.openCamera(option, videoView)
        if (ret != TXIoTError.SUCCESS) {
            toast("启动本地视频失败：$ret")
            return
        }
        isCameraOpened = true
        if (!isStreaming) {
            tvStatus.text = getString(R.string.monitor_status_connecting)
            tvHint.text = getString(R.string.monitor_preview_hint)
            tvHint.visibility = View.VISIBLE
        }
    }

    private fun closeCameraIfNeeded() {
        if (!isCameraOpened) return
        monitorSession.closeCamera()
        isCameraOpened = false
    }

    private fun openMicrophoneIfNeeded() {
        if (isTalking) {
            return
        }
        val ret = monitorSession.openMicrophone()
        if (ret != TXIoTError.SUCCESS) {
            toast("开启麦克风失败：$ret")
            return
        }
        isTalking = true
        Log.i(TAG, "openMicrophone success")
    }

    private fun closeMicrophoneIfNeeded() {
        if (!isTalking) {
            Log.i(TAG, "closeMicrophone skipped, not opened")
            return
        }
        monitorSession.closeMicrophone()
        isTalking = false
    }

    private fun stopMonitor() {
        closeMicrophoneIfNeeded()
        closeCameraIfNeeded()
    }

    private fun toggleTalk() {
        if (!isStreaming) {
            toast("当前无人观看，无需对讲")
            return
        }
        if (isTalking) {
            closeMicrophoneIfNeeded()
            toast("对讲已关闭")
        } else {
            openMicrophoneIfNeeded()
            if (isTalking) toast("对讲已开启")
        }
        refreshMicUi()
    }

    private fun refreshMicUi() {
        val whiteTint = android.content.res.ColorStateList.valueOf(0xFFFFFFFF.toInt())
        if (isTalking) {
            ivMicIcon.setBackgroundResource(R.drawable.bg_action_danger)
            ivMicIcon.setImageResource(R.drawable.ic_aitalk_mic)
            ivMicIcon.imageTintList = whiteTint
            tvMicLabel.setText(R.string.monitor_action_talk_on)
        } else {
            ivMicIcon.setBackgroundResource(R.drawable.bg_monitor_action_normal)
            ivMicIcon.setImageResource(R.drawable.ic_call_mic_off)
            ivMicIcon.imageTintList = whiteTint
            tvMicLabel.setText(R.string.monitor_action_talk)
        }
    }

    private fun toggleRatio() {
        if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            return
        }
        showRatioPopup()
    }

    private fun showRatioPopup() {
        val inflater = LayoutInflater.from(this)
        val root = inflater.inflate(R.layout.popup_monitor_ratio, null) as LinearLayout
        val popup = PopupWindow(
            root,
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
            true
        ).apply {
            setBackgroundDrawable(android.graphics.drawable.ColorDrawable(0))
            elevation = 12f * resources.displayMetrics.density
            isOutsideTouchable = true
            animationStyle = android.R.style.Animation_Dialog
        }
        ratioOptions.forEachIndexed { index, label ->
            val itemView = inflater.inflate(R.layout.item_monitor_ratio, root, false)
            val tv = itemView.findViewById<TextView>(R.id.tv_ratio_item_label)
            val iv = itemView.findViewById<ImageView>(R.id.iv_ratio_item_check)
            tv.text = label
            if (index == ratioIndex) {
                tv.setTextColor(0xFF4B7BF5.toInt())
                iv.visibility = View.VISIBLE
            } else {
                tv.setTextColor(0xFFFFFFFF.toInt())
                iv.visibility = View.INVISIBLE
            }
            itemView.setOnClickListener {
                popup.dismiss()
                if (index != ratioIndex) {
                    ratioIndex = index
                    tvRatio.text = ratioOptions[ratioIndex]
                    applyOrientationUi(resources.configuration.orientation)
                }
            }
            root.addView(itemView)
        }
        val offsetY = (4 * resources.displayMetrics.density).toInt()
        popup.showAsDropDown(tvRatio, 0, offsetY)
    }

    private fun toggleQuality() {
        val next = if (currentQuality == TXIoTMonitorSession.VideoQuality.HD) {
            TXIoTMonitorSession.VideoQuality.SD
        } else {
            TXIoTMonitorSession.VideoQuality.HD
        }
        val ret = monitorSession.switchVideoQuality(next)
        if (ret != TXIoTError.SUCCESS) {
            toast("切换画质失败：$ret")
            return
        }
        currentQuality = next
        tvQuality.text = qualityLabel(next)
    }

    private fun qualityLabel(quality: TXIoTMonitorSession.VideoQuality): String = when (quality) {
        TXIoTMonitorSession.VideoQuality.LD -> "流畅"
        TXIoTMonitorSession.VideoQuality.SD -> "标清"
        TXIoTMonitorSession.VideoQuality.HD -> getString(R.string.monitor_quality_hd)
        TXIoTMonitorSession.VideoQuality.FHD -> "超清"
        else -> "自动"
    }

    private fun toast(msg: String) {
        Log.i(TAG, "toast: $msg")
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}
