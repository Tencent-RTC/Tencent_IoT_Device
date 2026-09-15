package com.tencent.liteav.iot.demo

import android.content.Intent
import android.os.Bundle
import android.text.method.HideReturnsTransformationMethod
import android.text.method.PasswordTransformationMethod
import android.view.LayoutInflater
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.tencent.liteav.iot.TXIoTCallback
import com.tencent.liteav.iot.TXIoTDeviceEngine
import com.tencent.liteav.iot.demo.util.AppPreferences
import com.tencent.liteav.iot.demo.util.IoTSessionStore
import com.tencent.liteav.iot.TXIoTError

class LoginActivity : AppCompatActivity() {

    private lateinit var etProductId: EditText
    private lateinit var etDeviceId: EditText
    private lateinit var etDeviceSecret: EditText
    private lateinit var ivSecretToggle: ImageView
    private lateinit var flRegion: FrameLayout
    private lateinit var tvRegion: TextView
    private lateinit var btnLogin: Button
    private lateinit var ivLogo: ImageView

    private val regionOptions = listOf(
        "中国 (china)",
        "美国东部 (us-east)",
        "欧洲 (europe)",
        "亚太-曼谷 (ap-bangkok)"
    )

    private var selectedRegionIndex: Int = -1
    private var regionPopup: PopupWindow? = null

    private var secretVisible = false

    private var logoClickCount = 0
    private var logoLastClickTime = 0L

    private val engine: TXIoTDeviceEngine by lazy {
        TXIoTDeviceEngine.getInstance(applicationContext)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_login)

        bindViews()
        setupInteractions()
        applyRegionSelection(AppPreferences.region)
        autoLoginIfNeeded()
    }

    private fun bindViews() {
        etProductId = findViewById(R.id.et_product_id)
        etDeviceId = findViewById(R.id.et_device_id)
        etDeviceSecret = findViewById(R.id.et_device_secret)
        ivSecretToggle = findViewById(R.id.iv_secret_toggle)
        flRegion = findViewById(R.id.fl_region)
        tvRegion = findViewById(R.id.tv_region)
        btnLogin = findViewById(R.id.btn_login)
        ivLogo = findViewById(R.id.iv_login_logo)
    }

    private fun setupInteractions() {
        ivLogo.setOnClickListener {
            val now = System.currentTimeMillis()
            if (now - logoLastClickTime > 1500L) {
                logoClickCount = 0
            }
            logoLastClickTime = now
            logoClickCount++
            if (logoClickCount >= 3) {
                logoClickCount = 0
                startActivity(Intent(this, DeviceLogActivity::class.java))
            }
        }

        ivSecretToggle.setOnClickListener {
            secretVisible = !secretVisible
            if (secretVisible) {
                etDeviceSecret.transformationMethod =
                    HideReturnsTransformationMethod.getInstance()
            } else {
                etDeviceSecret.transformationMethod =
                    PasswordTransformationMethod.getInstance()
            }
            ivSecretToggle.isSelected = secretVisible
            etDeviceSecret.setSelection(etDeviceSecret.text?.length ?: 0)
        }

        flRegion.setOnClickListener(::showRegionMenu)

        btnLogin.setOnClickListener {
            val productId = etProductId.text.toString().trim()
            val deviceId = etDeviceId.text.toString().trim()
            val deviceSecret = etDeviceSecret.text.toString().trim()
            val regionText = tvRegion.text.toString().trim()

            when {
                productId.isEmpty() -> toast("请输入 ProductId")
                deviceId.isEmpty() -> toast("请输入 DeviceId")
                deviceSecret.isEmpty() -> toast("请输入 DeviceSecret")
                regionText.isEmpty() || regionText == getString(R.string.login_hint_region) ->
                    toast("请选择 Region")

                else -> doLogin(productId, deviceId, deviceSecret, parseRegion(regionText))
            }
        }
    }

    private fun applyRegionSelection(region: String) {
        val publicRegion = DemoApplication.publicRegion(region)
        val index = regionOptions.indexOfFirst { parseRegion(it) == publicRegion }
        selectedRegionIndex = if (index >= 0) index else 0
        tvRegion.text = regionOptions[selectedRegionIndex]
        tvRegion.setTextColor(
            ContextCompat.getColor(this, R.color.login_input_text)
        )
    }

    private fun autoLoginIfNeeded() {
        if (!AppPreferences.isLoggedIn) return
        etProductId.setText(AppPreferences.productId)
        etDeviceId.setText(AppPreferences.deviceId)
        etDeviceSecret.setText(AppPreferences.deviceSecret)
        val region = DemoApplication.publicRegion(AppPreferences.region)
        doLogin(
            AppPreferences.productId,
            AppPreferences.deviceId,
            AppPreferences.deviceSecret,
            region
        )
    }

    private fun doLogin(
        productId: String,
        deviceId: String,
        deviceSecret: String,
        region: String
    ) {
        val app = application as DemoApplication
        val initCode = app.ensureRegion(region)
        if (initCode != TXIoTError.SUCCESS && initCode != TXIoTError.ALREADY_INITIALIZED) {
            toast("初始化失败：$initCode")
            return
        }

        val deviceInfo = TXIoTDeviceEngine.DeviceInfo().apply {
            this.productId = productId
            this.deviceId = deviceId
            this.deviceSecret = deviceSecret
        }

        btnLogin.isEnabled = false
        toast("登录中...")

        engine.login(deviceInfo, object : TXIoTCallback {
            override fun onSuccess() {
                if (isFinishing || isDestroyed) return
                btnLogin.isEnabled = true
                IoTSessionStore.update(productId, deviceId, region)
                AppPreferences.saveLogin(productId, deviceId, deviceSecret, region)
                toast("登录成功")
                val intent = Intent(this@LoginActivity, HomeActivity::class.java).apply {
                    putExtra(HomeActivity.EXTRA_PRODUCT_ID, productId)
                    putExtra(HomeActivity.EXTRA_DEVICE_NAME, deviceId)
                    putExtra(HomeActivity.EXTRA_REGION, region)
                }
                startActivity(intent)
                finish()
            }

            override fun onError(code: Int, desc: String?) {
                if (isFinishing || isDestroyed) return
                btnLogin.isEnabled = true
                toast("登录失败：$code ${desc ?: ""}")
            }
        })
    }

    private fun parseRegion(display: String): String {
        val start = display.lastIndexOf('(')
        val end = display.lastIndexOf(')')
        return if (start in 0 until end) {
            display.substring(start + 1, end).trim()
        } else {
            display
        }
    }

    private fun showRegionMenu(anchor: View) {
        regionPopup?.dismiss()

        val inflater = LayoutInflater.from(this)
        val root = inflater.inflate(R.layout.popup_login_region, null)
        val container = root.findViewById<LinearLayout>(R.id.ll_login_region_container)

        regionOptions.forEachIndexed { index, name ->
            val item = inflater.inflate(R.layout.item_login_region, container, false)
            val tvName = item.findViewById<TextView>(R.id.tv_login_region_item)
            val ivCheck = item.findViewById<ImageView>(R.id.iv_login_region_check)

            tvName.text = name
            if (index == selectedRegionIndex) {
                tvName.setTextColor(0xFF1E64FF.toInt())
                ivCheck.visibility = View.VISIBLE
            } else {
                tvName.setTextColor(
                    ContextCompat.getColor(this, R.color.login_input_text)
                )
                ivCheck.visibility = View.GONE
            }

            item.setOnClickListener {
                selectedRegionIndex = index
                tvRegion.text = name
                tvRegion.setTextColor(
                    ContextCompat.getColor(this, R.color.login_input_text)
                )
                regionPopup?.dismiss()
            }
            container.addView(item)
        }

        val widthSpec = View.MeasureSpec.makeMeasureSpec(
            anchor.width, View.MeasureSpec.EXACTLY
        )
        val heightSpec = View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED)
        root.measure(widthSpec, heightSpec)
        val screenH = resources.displayMetrics.heightPixels
        val maxH = (screenH * 0.6f).toInt()
        val popupHeight = minOf(root.measuredHeight, maxH)

        val popup = PopupWindow(
            root,
            anchor.width,
            popupHeight,
            true
        ).apply {
            setBackgroundDrawable(android.graphics.drawable.ColorDrawable(0))
            isOutsideTouchable = true
            elevation = 24f
            animationStyle = android.R.style.Animation_Dialog
        }
        regionPopup = popup

        val yOffset = (4 * resources.displayMetrics.density).toInt()
        popup.showAsDropDown(anchor, 0, yOffset)
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}