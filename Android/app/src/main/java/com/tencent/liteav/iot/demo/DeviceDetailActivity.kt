package com.tencent.liteav.iot.demo

import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.SwitchCompat
import com.tencent.liteav.iot.TXIoTCallback
import com.tencent.liteav.iot.TXIoTDeviceEngine

class DeviceDetailActivity : CallAwareActivity() {

    companion object {
        private const val TAG = "DeviceDetailActivity"

        const val EXTRA_PRODUCT_ID = "extra_product_id"
        const val EXTRA_DEVICE_NAME = "extra_device_name"
        const val EXTRA_REGION = "extra_region"
    }

    private lateinit var tvProductId: TextView
    private lateinit var tvDeviceId: TextView
    private lateinit var tvRegion: TextView
    private lateinit var tvStateBadge: TextView
    private lateinit var btnLogout: Button
    private lateinit var ivBack: ImageView
    private lateinit var switchAutoLaunchPreview: SwitchCompat

    private val engine: TXIoTDeviceEngine by lazy {
        TXIoTDeviceEngine.getInstance(applicationContext)
    }

    private val deviceStateObserver: (TXIoTDeviceEngine.DeviceState) -> Unit = { state ->
        state.applyBadge(tvStateBadge)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_device_detail)

        tvProductId = findViewById(R.id.tv_product_id)
        tvDeviceId = findViewById(R.id.tv_device_id)
        tvRegion = findViewById(R.id.tv_region)
        tvStateBadge = findViewById(R.id.tv_state_badge)
        btnLogout = findViewById(R.id.btn_logout)
        ivBack = findViewById(R.id.iv_back)
        switchAutoLaunchPreview = findViewById(R.id.switch_auto_launch_preview)

        tvProductId.text = intent?.getStringExtra(EXTRA_PRODUCT_ID).ifEmptyText()
        tvDeviceId.text = intent?.getStringExtra(EXTRA_DEVICE_NAME).ifEmptyText()
        tvRegion.text = intent?.getStringExtra(EXTRA_REGION).ifEmptyText()

        DeviceStateObservable.addObserver(deviceStateObserver)

        setupMonitorPreference()

        ivBack.setOnClickListener { finish() }
        btnLogout.setOnClickListener { confirmLogout() }
    }

    private fun setupMonitorPreference() {
        switchAutoLaunchPreview.isChecked = MonitorPreferences.autoLaunchPreview
        switchAutoLaunchPreview.setOnCheckedChangeListener { _, isChecked ->
            MonitorPreferences.autoLaunchPreview = isChecked
        }
    }

    override fun onDestroy() {
        DeviceStateObservable.removeObserver(deviceStateObserver)
        super.onDestroy()
    }

    private fun confirmLogout() {
        val view = layoutInflater.inflate(R.layout.dialog_logout_confirm, null)
        val dialog = AlertDialog.Builder(this)
            .setView(view)
            .setCancelable(true)
            .create()
        dialog.window?.setBackgroundDrawable(
            android.graphics.drawable.ColorDrawable(0)
        )
        view.findViewById<android.view.View>(R.id.btn_logout_cancel).setOnClickListener {
            dialog.dismiss()
        }
        view.findViewById<android.view.View>(R.id.btn_logout_ok).setOnClickListener {
            dialog.dismiss()
            doLogout()
        }
        dialog.show()
    }

    private fun doLogout() {
        btnLogout.isEnabled = false
        engine.logout(object : TXIoTCallback {
            override fun onSuccess() {
                IoTSessionStore.clear()
                if (isFinishing || isDestroyed) return
                Toast.makeText(
                    this@DeviceDetailActivity,
                    R.string.detail_logout_success,
                    Toast.LENGTH_SHORT
                ).show()
                backToLogin()
            }

            override fun onError(code: Int, desc: String?) {
                Log.w(TAG, "logout failed, code=$code, desc=$desc")
                IoTSessionStore.clear()
                if (isFinishing || isDestroyed) return
                Toast.makeText(
                    this@DeviceDetailActivity,
                    getString(R.string.detail_logout_failed, "$code ${desc.orEmpty()}"),
                    Toast.LENGTH_SHORT
                ).show()
                backToLogin()
            }
        })
    }

    private fun backToLogin() {
        val intent = Intent(this, LoginActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        }
        startActivity(intent)
        finish()
    }

    private fun String?.ifEmptyText(): String =
        if (this.isNullOrEmpty()) getString(R.string.detail_value_empty) else this
}
