package com.tencent.liteav.iot.demo

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.View
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.tencent.liteav.iot.TXIoTDeviceEngine
import com.tencent.liteav.iot.TXIoTError
import com.tencent.liteav.iot.TXIoTMonitorSession

class HomeActivity : CallAwareActivity() {

    companion object {
        const val EXTRA_PRODUCT_ID = "extra_product_id"
        const val EXTRA_DEVICE_NAME = "extra_device_name"
        const val EXTRA_REGION = "extra_region"

        private const val REQ_AV_PERM = 1001
    }

    private lateinit var tvDeviceState: TextView
    private lateinit var ivAvatar: ImageView
    private lateinit var llStreamingBanner: View

    private var productId: String = ""
    private var deviceId: String = ""
    private var region: String = ""

    private val deviceStateObserver: (TXIoTDeviceEngine.DeviceState) -> Unit = { state ->
        state.applyBadge(tvDeviceState)
    }

    private val monitorSession: TXIoTMonitorSession by lazy { TXIoTMonitorSession.getInstance() }
    private var monitorLaunched: Boolean = false

    private var headlessStreaming: Boolean = false

    private val globalMonitorListener = object : TXIoTMonitorSession.Listener {
        override fun onMonitorBegin(
            option: TXIoTMonitorSession.Option?,
            customData: String?
        ) {
            if (MonitorPreferences.autoLaunchPreview) {
                launchMonitorActivity(option, customData)
            } else {
                startHeadlessMonitor(option)
            }
        }

        override fun onMonitorEnd() {
            monitorLaunched = false
            stopHeadlessMonitor()
        }

        override fun onMonitorSwitch(quality: TXIoTMonitorSession.VideoQuality?) = Unit

        override fun onPTZCommandReceived(
            ptzCommand: TXIoTMonitorSession.PTZCommand?,
            speed: Int
        ) = Unit
    }
    
    private fun launchMonitorActivity(
        option: TXIoTMonitorSession.Option?,
        customData: String?
    ) {
        if (monitorLaunched) return
        monitorLaunched = true
        val qualityName = option?.videoQuality?.name.orEmpty()
        val intent = Intent(this@HomeActivity, MonitorActivity::class.java).apply {
            putExtra(MonitorActivity.EXTRA_QUALITY, qualityName)
            putExtra(MonitorActivity.EXTRA_CUSTOM_DATA, customData.orEmpty())
            addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
        }
        startActivity(intent)
    }

    private fun startHeadlessMonitor(option: TXIoTMonitorSession.Option?) {
        if (headlessStreaming) return
        val opt = TXIoTMonitorSession.Option().apply {
            videoQuality = option?.videoQuality
                ?: TXIoTMonitorSession.VideoQuality.HD
        }
        val camRet = monitorSession.openCamera(opt, null)
        if (camRet != TXIoTError.SUCCESS) {
            Toast.makeText(
                this@HomeActivity,
                "后台推流失败：$camRet",
                Toast.LENGTH_SHORT
            ).show()
            return
        }
        headlessStreaming = true
        monitorSession.openMicrophone()
        updateStreamingBanner()
    }

    private fun stopHeadlessMonitor() {
        if (!headlessStreaming) return
        headlessStreaming = false
        monitorSession.closeMicrophone()
        monitorSession.closeCamera()
        updateStreamingBanner()
    }

    private fun updateStreamingBanner() {
        llStreamingBanner.visibility = if (headlessStreaming) View.VISIBLE else View.GONE
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_home)

        productId = intent?.getStringExtra(EXTRA_PRODUCT_ID).orEmpty()
        deviceId = intent?.getStringExtra(EXTRA_DEVICE_NAME).orEmpty()
        region = intent?.getStringExtra(EXTRA_REGION).orEmpty()

        bindHeader()
        setupCards()
        IncomingCallBannerController.install()
        monitorSession.addListener(globalMonitorListener)
        DeviceStateObservable.addObserver(deviceStateObserver)
        ensureAvPermission()
    }

    private fun ensureAvPermission() {
        val perms = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        val missing = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) return
        ActivityCompat.requestPermissions(this, missing.toTypedArray(), REQ_AV_PERM)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_AV_PERM) return
        val denied = permissions.zip(grantResults.toTypedArray())
            .filter { it.second != PackageManager.PERMISSION_GRANTED }
            .map { it.first }
        if (denied.isNotEmpty()) {
            Toast.makeText(
                this,
                "未授予相机/麦克风权限，监控与通话功能将无法使用",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    override fun onDestroy() {
        monitorSession.removeListener(globalMonitorListener)
        DeviceStateObservable.removeObserver(deviceStateObserver)
        stopHeadlessMonitor()
        super.onDestroy()
    }

    private fun bindHeader() {
        tvDeviceState = findViewById(R.id.tv_device_state)
        ivAvatar = findViewById(R.id.iv_home_avatar)
        llStreamingBanner = findViewById(R.id.ll_home_streaming_banner)
        updateStreamingBanner()
        ivAvatar.setOnClickListener {
            val intent = Intent(this, DeviceDetailActivity::class.java).apply {
                putExtra(DeviceDetailActivity.EXTRA_PRODUCT_ID, productId)
                putExtra(DeviceDetailActivity.EXTRA_DEVICE_NAME, deviceId)
                putExtra(DeviceDetailActivity.EXTRA_REGION, region)
            }
            startActivity(intent)
        }
    }

    private fun setupCards() {
        findViewById<View>(R.id.card_aitalk).setOnClickListener {
            startActivity(Intent(this, AITalkActivity::class.java))
        }
        findViewById<View>(R.id.card_thing_model).setOnClickListener {
            startActivity(Intent(this, DataModelActivity::class.java))
        }
        findViewById<View>(R.id.card_twecall).setOnClickListener {
            startActivity(Intent(this, ContactsActivity::class.java))
        }
    }
}
