package com.tencent.liteav.iot.demo

import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.SwitchCompat
import com.tencent.liteav.iot.TXIoTCallback
import com.tencent.liteav.iot.TXIoTDeviceEngine
import com.tencent.liteav.iot.demo.util.AppPreferences
import com.tencent.liteav.iot.demo.util.DeviceSignatureRequest
import com.tencent.liteav.iot.demo.util.IoTSessionStore
import com.tencent.liteav.iot.demo.util.QrCodeGenerator

class DeviceDetailActivity : CallAwareActivity() {

    companion object {
        private const val TAG = "DeviceDetailActivity"

        const val EXTRA_PRODUCT_ID = "extra_product_id"
        const val EXTRA_DEVICE_NAME = "extra_device_name"
        const val EXTRA_REGION = "extra_region"

        private const val SIGNATURE_EXPIRE = 5 * 60
        private const val DEFAULT_REGION = "ap-guangzhou"
    }

    private lateinit var tvProductId: TextView
    private lateinit var tvDeviceId: TextView
    private lateinit var tvRegion: TextView
    private lateinit var tvStateBadge: TextView
    private lateinit var btnLogout: Button
    private lateinit var ivBack: ImageView
    private lateinit var switchAutoLaunchPreview: SwitchCompat
    private lateinit var llTweCallFlavor: View
    private lateinit var tvTweCallFlavorValue: TextView
    private lateinit var etSecretId: EditText
    private lateinit var etSecretKey: EditText
    private lateinit var btnGetSignature: Button
    private lateinit var tvSignatureConsoleLink: TextView
    private lateinit var tvSignatureDesc: TextView
    private lateinit var llSignatureResult: View
    private lateinit var ivSignatureQr: ImageView

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
        llTweCallFlavor = findViewById(R.id.ll_twecall_flavor)
        tvTweCallFlavorValue = findViewById(R.id.tv_twecall_flavor_value)
        etSecretId = findViewById(R.id.et_secret_id)
        etSecretKey = findViewById(R.id.et_secret_key)
        btnGetSignature = findViewById(R.id.btn_get_signature)
        tvSignatureConsoleLink = findViewById(R.id.tv_signature_console_link)
        tvSignatureDesc = findViewById(R.id.tv_signature_desc)
        llSignatureResult = findViewById(R.id.ll_signature_result)
        ivSignatureQr = findViewById(R.id.iv_signature_qr)

        tvProductId.text = intent?.getStringExtra(EXTRA_PRODUCT_ID).ifEmptyText()
        tvDeviceId.text = intent?.getStringExtra(EXTRA_DEVICE_NAME).ifEmptyText()
        tvRegion.text = intent?.getStringExtra(EXTRA_REGION).ifEmptyText()

        DeviceStateObservable.addObserver(deviceStateObserver)

        setupMonitorPreference()
        setupTweCallPreference()
        setupSignature()

        ivBack.setOnClickListener { finish() }
        btnLogout.setOnClickListener { confirmLogout() }
    }

    private fun setupMonitorPreference() {
        switchAutoLaunchPreview.isChecked = AppPreferences.autoLaunchPreview
        switchAutoLaunchPreview.setOnCheckedChangeListener { _, isChecked ->
            AppPreferences.autoLaunchPreview = isChecked
        }
    }

    private fun setupTweCallPreference() {
        tvTweCallFlavorValue.text = flavorToLabel(AppPreferences.miniProgramFlavor)
        llTweCallFlavor.setOnClickListener { showFlavorPicker() }
    }

    private fun setupSignature() {
        etSecretId.setText(AppPreferences.secretId)
        etSecretKey.setText(AppPreferences.secretKey)
        btnGetSignature.setOnClickListener { requestSignature() }
        ivSignatureQr.setOnClickListener { requestSignature() }
        tvSignatureConsoleLink.setOnClickListener { openConsole() }

        // 已经保存过 SecretId / SecretKey 时，进入页面直接获取并展示二维码
        if (AppPreferences.secretId.isNotEmpty() && AppPreferences.secretKey.isNotEmpty()) {
            requestSignature()
        }
    }

    private fun openConsole() {
        val url = getString(R.string.detail_signature_console_url)
        try {
            startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(url)))
        } catch (e: Exception) {
            Log.w(TAG, "open console failed", e)
        }
    }

    private fun requestSignature() {
        val secretId = etSecretId.text.toString().trim()
        val secretKey = etSecretKey.text.toString().trim()
        val productId = (intent?.getStringExtra(EXTRA_PRODUCT_ID).orEmpty())
            .ifEmpty { AppPreferences.productId }
        val deviceName = (intent?.getStringExtra(EXTRA_DEVICE_NAME).orEmpty())
            .ifEmpty { AppPreferences.deviceId }
        val region = (intent?.getStringExtra(EXTRA_REGION).orEmpty())
            .ifEmpty { AppPreferences.region }

        when {
            secretId.isEmpty() -> {
                toast(getString(R.string.detail_signature_empty_secret_id))
                return
            }
            secretKey.isEmpty() -> {
                toast(getString(R.string.detail_signature_empty_secret_key))
                return
            }
            productId.isEmpty() || deviceName.isEmpty() -> {
                toast(getString(R.string.detail_signature_no_device))
                return
            }
        }

        btnGetSignature.isEnabled = false
        btnGetSignature.text = getString(R.string.detail_signature_getting)

        AppPreferences.secretId = secretId
        AppPreferences.secretKey = secretKey

        DeviceSignatureRequest.request(
            secretId = secretId,
            secretKey = secretKey,
            productId = productId,
            deviceName = deviceName,
            expire = SIGNATURE_EXPIRE,
            region = region.ifEmpty { DEFAULT_REGION },
            callback = object : DeviceSignatureRequest.Callback {
                override fun onSuccess(result: DeviceSignatureRequest.DeviceSignatureResult) {
                    if (isFinishing || isDestroyed) return
                    restoreSignatureButton()
                    llSignatureResult.visibility = View.VISIBLE
                    val json = org.json.JSONObject().apply {
                        put("DeviceName", result.deviceName.ifEmpty { deviceName })
                        put("ProductId", productId)
                        put("Signature", result.deviceSignature)
                    }
                    renderQrCode(json.toString())
                    showQrState(true)
                }

                override fun onError(code: String, message: String) {
                    if (isFinishing || isDestroyed) return
                    restoreSignatureButton()
                    Log.w(TAG, "get signature failed, code=$code, msg=$message")
                    toast(getString(R.string.detail_signature_failed, "$code $message".trim()))
                }
            }
        )
    }

    private fun restoreSignatureButton() {
        btnGetSignature.isEnabled = true
        btnGetSignature.text = getString(R.string.detail_signature_get)
    }

    private fun renderQrCode(content: String) {
        try {
            val sizePx = (resources.displayMetrics.density * 220).toInt()
            val bitmap = QrCodeGenerator.encode(content, sizePx)
            ivSignatureQr.setImageBitmap(bitmap)
        } catch (e: Exception) {
            Log.w(TAG, "render qr code failed", e)
            toast(getString(R.string.detail_signature_failed, e.message.orEmpty()))
        }
    }

    /** 切换二维码展示态与密钥输入态。 */
    private fun showQrState(showing: Boolean) {
        val inputVisibility = if (showing) View.GONE else View.VISIBLE
        tvSignatureConsoleLink.visibility = inputVisibility
        etSecretId.visibility = inputVisibility
        etSecretKey.visibility = inputVisibility
        btnGetSignature.visibility = inputVisibility
        tvSignatureDesc.setText(
            if (showing) R.string.detail_signature_desc_valid
            else R.string.detail_signature_desc
        )
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }

    private fun showFlavorPicker() {
        val flavors = listOf(
            FlavorOption(
                AppPreferences.FLAVOR_RELEASE,
                getString(R.string.detail_twecall_flavor_release),
                getString(R.string.detail_twecall_flavor_release_desc),
                R.drawable.dot_flavor_release
            ),
            FlavorOption(
                AppPreferences.FLAVOR_TRIAL,
                getString(R.string.detail_twecall_flavor_trial),
                getString(R.string.detail_twecall_flavor_trial_desc),
                R.drawable.dot_flavor_trial
            ),
            FlavorOption(
                AppPreferences.FLAVOR_DEBUG,
                getString(R.string.detail_twecall_flavor_debug),
                getString(R.string.detail_twecall_flavor_debug_desc),
                R.drawable.dot_flavor_debug
            )
        )
        val current = AppPreferences.miniProgramFlavor

        val popup = androidx.appcompat.widget.ListPopupWindow(this)
        popup.anchorView = llTweCallFlavor
        popup.setBackgroundDrawable(
            androidx.core.content.ContextCompat.getDrawable(this, R.drawable.bg_flavor_dropdown)
        )
        popup.width = (resources.displayMetrics.density * 260).toInt()
        popup.isModal = true
        popup.horizontalOffset = 0
        popup.verticalOffset = (resources.displayMetrics.density * 8).toInt()

        val adapter = object : android.widget.BaseAdapter() {
            override fun getCount(): Int = flavors.size
            override fun getItem(position: Int): Any = flavors[position]
            override fun getItemId(position: Int): Long = position.toLong()
            override fun getView(position: Int, convertView: View?, parent: android.view.ViewGroup): View {
                val view = convertView ?: LayoutInflater.from(this@DeviceDetailActivity)
                    .inflate(R.layout.item_flavor_dropdown, parent, false)
                val option = flavors[position]
                view.findViewById<View>(R.id.v_flavor_dot).setBackgroundResource(option.dotRes)
                view.findViewById<TextView>(R.id.tv_flavor_title).text = option.title
                view.findViewById<TextView>(R.id.tv_flavor_subtitle).text = option.subtitle
                view.findViewById<ImageView>(R.id.iv_flavor_check).visibility =
                    if (option.flavor == current) View.VISIBLE else View.INVISIBLE
                return view
            }
        }
        popup.setAdapter(adapter)

        popup.setOnItemClickListener { _, _, position, _ ->
            popup.dismiss()
            val option = flavors[position]
            val changed = AppPreferences.miniProgramFlavor != option.flavor
            AppPreferences.miniProgramFlavor = option.flavor
            tvTweCallFlavorValue.text = option.title
            if (changed) {
                Toast.makeText(
                    this,
                    R.string.detail_twecall_flavor_restart_tip,
                    Toast.LENGTH_SHORT
                ).show()
            }
        }
        popup.show()
    }

    private data class FlavorOption(
        val flavor: String,
        val title: String,
        val subtitle: String,
        val dotRes: Int
    )

    private fun flavorToLabel(flavor: String): String = when (flavor) {
        AppPreferences.FLAVOR_TRIAL -> getString(R.string.detail_twecall_flavor_trial)
        AppPreferences.FLAVOR_DEBUG -> getString(R.string.detail_twecall_flavor_debug)
        else -> getString(R.string.detail_twecall_flavor_release)
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
                AppPreferences.clearLogin()
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
                AppPreferences.clearLogin()
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
