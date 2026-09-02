package com.tencent.liteav.iot.demo

import android.os.Build
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.LayoutInflater
import android.view.View
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.annotation.RequiresApi
import com.tencent.liteav.iot.TXIoTDataModelSession
import com.tencent.liteav.iot.TXIoTDataModelSession.Action
import com.tencent.liteav.iot.TXIoTDataModelSession.Data
import com.tencent.liteav.iot.TXIoTDataModelSession.DataType
import com.tencent.liteav.iot.TXIoTDataModelSession.DataValue
import com.tencent.liteav.iot.TXIoTDataModelSession.Event
import com.tencent.liteav.iot.TXIoTDataModelSession.EventType
import com.tencent.liteav.iot.TXIoTDataModelSession.ResultItem
import com.tencent.liteav.iot.TXIoTCallback
import com.tencent.liteav.iot.TXIoTDeviceEngine
import com.tencent.liteav.iot.TXIoTValueCallback

class DataModelActivity : CallAwareActivity() {

    private lateinit var ivBack: ImageView
    private lateinit var tvDeviceName: TextView
    private lateinit var tvDeviceState: TextView
    private lateinit var tvDeviceProduct: TextView
    private lateinit var tvDeviceDevice: TextView
    private lateinit var tabProperty: LinearLayout
    private lateinit var tabEvent: LinearLayout
    private lateinit var tvTabProperty: TextView
    private lateinit var tvTabEvent: TextView
    private lateinit var indicatorProperty: View
    private lateinit var indicatorEvent: View
    private lateinit var etSearch: EditText
    private lateinit var llFilter: LinearLayout
    private lateinit var llList: LinearLayout
    private lateinit var btnAdd: TextView

    /** 全部属性数据（可编辑） */
    private val properties: MutableList<PropertyModel> = mutableListOf()

    /** 全部事件数据 */
    private val events: MutableList<EventModel> = mutableListOf()

    /** 当前展开的属性索引（Property Tab 下），-1 表示无展开项 */
    private var expandedIndex: Int = 0

    /** 当前 Tab：true=属性，false=事件 */
    private var isPropertyTab: Boolean = true

    /** 搜索关键字 */
    private var keyword: String = ""

    private val dataModelSession: TXIoTDataModelSession by lazy { TXIoTDataModelSession.getInstance() }
    private val engine: TXIoTDeviceEngine by lazy { TXIoTDeviceEngine.getInstance(applicationContext) }

    private val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())

    private val dataModelListener = object : TXIoTDataModelSession.Listener {
        override fun onReceivePropertyChanged(property: Data?) {
            // SDK 已切主线程
            property ?: return
            applyRemotePropertyUpdate(property)
        }

        override fun onReceiveNewAction(action: Action?): Int {
            // SDK 未切主线程，需自行 post。返回 0 表示"处理成功"。
            if (action == null) return 0
            mainHandler.post { handleReceivedAction(action) }
            return 0
        }
    }

    private val engineListener: (TXIoTDeviceEngine.DeviceState) -> Unit = { state ->
        // DeviceStateCenter 回调已在主线程。
        deviceState = state
        applyDeviceStateUi()
    }

    /** 当前设备连接状态（由 SDK 回调更新）。 */
    private var deviceState: TXIoTDeviceEngine.DeviceState = TXIoTDeviceEngine.DeviceState.OFFLINE

    @RequiresApi(Build.VERSION_CODES.M)
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_thing_model)

        bindViews()
        setupDeviceCard()
        setupTabs()
        setupSearch()
        setupBottomButton()

        // 设备端物模型 Schema：SDK 未提供 schema 查询接口，schema 必须由设备端
        // 本地定义。这里初始化 3 个属性 + 1 个事件，与真实的腾讯云 IoT 物模型对齐。
        initLocalSchema()

        // 挂载 SDK 监听：属性变更 / 云端 Action；在线状态统一由 DeviceStateCenter 回放。
        dataModelSession.addListener(dataModelListener)
        DeviceStateObservable.addObserver(engineListener)

        refreshTabCount()
        refreshList()

        // 页面进入时，主动把当前所有属性上报一次，让云端拿到设备的最新状态。
        // 这是设备端 SDK 的常规行为（"上电即上报"）。
        if (isLoggedIn()) {
            syncReportAllProperties(silent = true)
        }
    }

    override fun onDestroy() {
        dataModelSession.removeListener(dataModelListener)
        DeviceStateObservable.removeObserver(engineListener)
        mainHandler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    private fun bindViews() {
        ivBack = findViewById(R.id.iv_back)
        tvDeviceName = findViewById(R.id.tv_device_name)
        tvDeviceState = findViewById(R.id.tv_device_state)
        tvDeviceProduct = findViewById(R.id.tv_device_product)
        tvDeviceDevice = findViewById(R.id.tv_device_device)
        tabProperty = findViewById(R.id.tab_property)
        tabEvent = findViewById(R.id.tab_event)
        tvTabProperty = findViewById(R.id.tv_tab_property)
        tvTabEvent = findViewById(R.id.tv_tab_event)
        indicatorProperty = findViewById(R.id.indicator_property)
        indicatorEvent = findViewById(R.id.indicator_event)
        etSearch = findViewById(R.id.et_search)
        llFilter = findViewById(R.id.ll_filter)
        llList = findViewById(R.id.ll_list)
        btnAdd = findViewById(R.id.btn_add)

        ivBack.setOnClickListener { finish() }
    }

    private fun setupDeviceCard() {
        val productId = IoTSessionStore.productId
        val deviceId = IoTSessionStore.deviceId
        tvDeviceProduct.text = getString(R.string.thing_device_product_prefix) +
                productId.ifEmpty { "Android Product" }
        tvDeviceDevice.text = getString(R.string.thing_device_device_prefix) +
                deviceId.ifEmpty { "android_demo_device_001" }
    }

    /** 根据 SDK 上报的设备状态刷新顶部胶囊。 */
    @RequiresApi(Build.VERSION_CODES.M)
    private fun applyDeviceStateUi() {
        when (deviceState) {
            TXIoTDeviceEngine.DeviceState.ONLINE -> {
                tvDeviceState.text = getString(R.string.thing_state_online)
                tvDeviceState.setTextColor(getColor(R.color.thing_online_text))
                tvDeviceState.setBackgroundResource(R.drawable.bg_thing_online_chip)
            }
            TXIoTDeviceEngine.DeviceState.RECONNECTING -> {
                tvDeviceState.text = getString(R.string.thing_state_reconnecting)
                tvDeviceState.setTextColor(getColor(R.color.thing_avatar_enum_text))
                tvDeviceState.setBackgroundResource(R.drawable.bg_thing_avatar_enum)
            }
            TXIoTDeviceEngine.DeviceState.OFFLINE -> {
                tvDeviceState.text = getString(R.string.thing_state_offline)
                tvDeviceState.setTextColor(getColor(R.color.thing_subtitle))
                tvDeviceState.setBackgroundResource(R.drawable.bg_thing_type_chip)
            }
        }
    }

    private fun isLoggedIn(): Boolean = IoTSessionStore.productId.isNotEmpty() &&
            IoTSessionStore.deviceId.isNotEmpty()

    @RequiresApi(Build.VERSION_CODES.M)
    private fun setupTabs() {
        tabProperty.setOnClickListener { switchTab(true) }
        tabEvent.setOnClickListener { switchTab(false) }
    }

    @RequiresApi(Build.VERSION_CODES.M)
    private fun switchTab(property: Boolean) {
        if (isPropertyTab == property) return
        isPropertyTab = property
        applyTabStyle()
        refreshList()
    }

    @RequiresApi(Build.VERSION_CODES.M)
    private fun applyTabStyle() {
        val selectedColor = getColor(R.color.thing_tab_selected)
        val unselectedColor = getColor(R.color.thing_tab_unselected)
        val transparent = 0x00000000
        if (isPropertyTab) {
            tvTabProperty.setTextColor(selectedColor)
            tvTabProperty.setTypeface(null, android.graphics.Typeface.BOLD)
            indicatorProperty.setBackgroundColor(selectedColor)
            tvTabEvent.setTextColor(unselectedColor)
            tvTabEvent.setTypeface(null, android.graphics.Typeface.NORMAL)
            indicatorEvent.setBackgroundColor(transparent)
        } else {
            tvTabProperty.setTextColor(unselectedColor)
            tvTabProperty.setTypeface(null, android.graphics.Typeface.NORMAL)
            indicatorProperty.setBackgroundColor(transparent)
            tvTabEvent.setTextColor(selectedColor)
            tvTabEvent.setTypeface(null, android.graphics.Typeface.BOLD)
            indicatorEvent.setBackgroundColor(selectedColor)
        }
    }

    private fun refreshTabCount() {
        tvTabProperty.text = "${getString(R.string.thing_tab_property)}（${properties.size}）"
        tvTabEvent.text = "${getString(R.string.thing_tab_event)}（${events.size}）"
    }

    private fun setupSearch() {
        etSearch.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                keyword = s?.toString()?.trim().orEmpty()
                refreshList()
            }
            override fun afterTextChanged(s: Editable?) = Unit
        })
        llFilter.setOnClickListener { toast(getString(R.string.thing_filter)) }
    }

    private fun setupBottomButton() {
        btnAdd.setOnClickListener { toast(getString(R.string.thing_btn_add)) }
    }

    // ==================== 设备端物模型 Schema ====================

    /**
     * 定义设备端本地物模型：属性 & 事件。
     * SDK 未提供 schema 查询接口，所有物模型元信息必须由设备端自行维护。
     */
    private fun initLocalSchema() {
        properties.clear()
        properties.add(
            PropertyModel(
                identifier = "welcome_word",
                displayName = "欢迎语",
                type = DataType.STRING,
                readWrite = "rw",
                rangeText = "0 ~ 2048 (string)",
                maxLength = 2048,
                currentValue = "欢迎使用 Android IoT 设备"
            )
        )
        properties.add(
            PropertyModel(
                identifier = "battery_level",
                displayName = "电量等级",
                type = DataType.INT,
                readWrite = "r",
                rangeText = "0 ~ 100 (int)",
                maxLength = 3,
                currentValue = "85"
            )
        )
        properties.add(
            PropertyModel(
                identifier = "firmware_version",
                displayName = "固件版本",
                type = DataType.STRING,
                readWrite = "r",
                rangeText = "0 ~ 64 (string)",
                maxLength = 64,
                currentValue = "1.0.3"
            )
        )

        events.clear()
        events.add(
            EventModel(
                identifier = "low_battery_alert",
                displayName = "低电量告警",
                type = EventType.ALERT
            )
        )
    }

    // ==================== 渲染列表 ====================

    private fun refreshList() {
        refreshTabCount()
        llList.removeAllViews()
        if (isPropertyTab) {
            val filtered = properties
                .withIndex()
                .filter {
                    keyword.isEmpty() ||
                            it.value.identifier.contains(keyword, ignoreCase = true) ||
                            it.value.displayName.contains(keyword)
                }
            filtered.forEachIndexed { pos, indexed ->
                val realIndex = indexed.index
                val expanded = realIndex == expandedIndex
                val view = if (expanded) {
                    buildExpandedItem(indexed.value, realIndex)
                } else {
                    buildCollapsedItem(indexed.value, realIndex, showDivider = pos != filtered.lastIndex)
                }
                llList.addView(view)
            }
        } else {
            events.forEach { event ->
                llList.addView(buildEventItem(event))
            }
        }
    }

    private fun buildExpandedItem(model: PropertyModel, index: Int): View {
        val view = LayoutInflater.from(this)
            .inflate(R.layout.item_thing_property_expanded, llList, false)

        val tvAvatar = view.findViewById<TextView>(R.id.tv_avatar)
        val avatarBg = tvAvatar.parent as View
        val tvName = view.findViewById<TextView>(R.id.tv_name)
        val tvType = view.findViewById<TextView>(R.id.tv_type)
        val tvDisplayName = view.findViewById<TextView>(R.id.tv_display_name)
        val tvMetaId = view.findViewById<TextView>(R.id.tv_meta_identifier)
        val tvMetaRw = view.findViewById<TextView>(R.id.tv_meta_rw)
        val tvMetaRange = view.findViewById<TextView>(R.id.tv_meta_range)
        val etValue = view.findViewById<EditText>(R.id.et_value)
        val tvLength = view.findViewById<TextView>(R.id.tv_length)
        val btnGet = view.findViewById<TextView>(R.id.btn_get)
        val btnReport = view.findViewById<TextView>(R.id.btn_report)

        applyAvatarStyle(avatarBg, tvAvatar, model.type)
        tvName.text = model.identifier
        tvType.text = typeLabel(model.type)
        tvDisplayName.text = model.displayName
        tvMetaId.text = getString(R.string.thing_label_identifier) + model.identifier
        tvMetaRw.text = getString(R.string.thing_label_rw) + model.readWrite
        tvMetaRange.text = getString(R.string.thing_label_range) + model.rangeText

        // 输入类型
        etValue.inputType = when (model.type) {
            DataType.INT -> android.text.InputType.TYPE_CLASS_NUMBER
            DataType.FLOAT -> android.text.InputType.TYPE_CLASS_NUMBER or
                    android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL
            else -> android.text.InputType.TYPE_CLASS_TEXT
        }
        etValue.setText(model.currentValue)
        tvLength.text = "${model.currentValue.length} / ${model.maxLength}"

        etValue.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                val text = s?.toString().orEmpty()
                model.currentValue = text
                tvLength.text = "${text.length} / ${model.maxLength}"
            }
            override fun afterTextChanged(s: Editable?) = Unit
        })

        // 点击首行区域切换收起
        view.findViewById<View>(R.id.tv_name).setOnClickListener {
            expandedIndex = if (expandedIndex == index) -1 else index
            refreshList()
        }

        btnGet.setOnClickListener { doReport(model, isGet = true) }
        btnReport.setOnClickListener { doReport(model, isGet = false) }

        return view
    }

    private fun buildCollapsedItem(model: PropertyModel, index: Int, showDivider: Boolean): View {
        val view = LayoutInflater.from(this)
            .inflate(R.layout.item_thing_property_collapsed, llList, false)

        val tvAvatar = view.findViewById<TextView>(R.id.tv_avatar)
        val avatarBg = tvAvatar.parent as View
        val tvName = view.findViewById<TextView>(R.id.tv_name)
        val tvType = view.findViewById<TextView>(R.id.tv_type)
        val tvDisplayName = view.findViewById<TextView>(R.id.tv_display_name)
        val tvMetaId = view.findViewById<TextView>(R.id.tv_meta_identifier)
        val tvMetaRw = view.findViewById<TextView>(R.id.tv_meta_rw)
        val tvMetaRange = view.findViewById<TextView>(R.id.tv_meta_range)
        val tvCurrent = view.findViewById<TextView>(R.id.tv_current_value)
        val divider = view.findViewById<View>(R.id.divider)

        applyAvatarStyle(avatarBg, tvAvatar, model.type)
        tvName.text = model.identifier
        tvType.text = typeLabel(model.type)
        tvDisplayName.text = model.displayName
        tvMetaId.text = getString(R.string.thing_label_identifier) + model.identifier
        tvMetaRw.text = getString(R.string.thing_label_rw) + model.readWrite
        tvMetaRange.text = getString(R.string.thing_label_range) + model.rangeText
        tvCurrent.text = getString(R.string.thing_label_current_value_prefix) + model.currentValue
        divider.visibility = if (showDivider) View.VISIBLE else View.GONE

        view.setOnClickListener {
            expandedIndex = index
            refreshList()
        }
        return view
    }

    private fun buildEventItem(model: EventModel): View {
        val view = LayoutInflater.from(this)
            .inflate(R.layout.item_thing_event, llList, false)
        val tvAvatar = view.findViewById<TextView>(R.id.tv_avatar)
        val avatarBg = tvAvatar.parent as View
        val tvName = view.findViewById<TextView>(R.id.tv_name)
        val tvDisplayName = view.findViewById<TextView>(R.id.tv_display_name)
        val tvMetaId = view.findViewById<TextView>(R.id.tv_meta_identifier)
        val tvMetaType = view.findViewById<TextView>(R.id.tv_meta_type)
        val btnReport = view.findViewById<TextView>(R.id.btn_report_event)

        avatarBg.setBackgroundResource(R.drawable.bg_thing_avatar_enum)
        tvAvatar.setTextColor(getColor(R.color.thing_avatar_enum_text))
        tvAvatar.text = "E"

        tvName.text = model.identifier
        tvDisplayName.text = model.displayName
        tvMetaId.text = getString(R.string.thing_label_identifier) + model.identifier
        tvMetaType.text = "事件类型: ${model.type.name.lowercase()}"

        btnReport.setOnClickListener { doReportEvent(model) }
        return view
    }

    private fun applyAvatarStyle(bg: View, tv: TextView, type: DataType) {
        when (type) {
            DataType.STRING, DataType.TIME -> {
                bg.setBackgroundResource(R.drawable.bg_thing_avatar_string)
                tv.setTextColor(getColor(R.color.thing_avatar_string_text))
                tv.text = "T"
            }
            DataType.INT, DataType.FLOAT -> {
                bg.setBackgroundResource(R.drawable.bg_thing_avatar_int)
                tv.setTextColor(getColor(R.color.thing_avatar_int_text))
                tv.text = "N"
            }
            DataType.BOOL -> {
                bg.setBackgroundResource(R.drawable.bg_thing_avatar_bool)
                tv.setTextColor(getColor(R.color.thing_avatar_bool_text))
                tv.text = "B"
            }
            DataType.ENUM -> {
                bg.setBackgroundResource(R.drawable.bg_thing_avatar_enum)
                tv.setTextColor(getColor(R.color.thing_avatar_enum_text))
                tv.text = "E"
            }
        }
    }

    private fun typeLabel(type: DataType): String = when (type) {
        DataType.STRING -> getString(R.string.thing_type_string)
        DataType.INT -> getString(R.string.thing_type_int)
        DataType.BOOL -> getString(R.string.thing_type_bool)
        DataType.FLOAT -> getString(R.string.thing_type_float)
        DataType.ENUM -> getString(R.string.thing_type_enum)
        DataType.TIME -> "time"
    }

    // ==================== SDK 调用 ====================

    private fun doReport(model: PropertyModel, isGet: Boolean) {
        if (!isLoggedIn()) {
            toast("请先登录设备")
            return
        }
        val item = buildDataItem(model) ?: run {
            toast("当前值格式不正确")
            return
        }
        val tip = if (isGet) "获取" else "上报"
        dataModelSession.reportProperty(listOf(item), object : TXIoTValueCallback<List<ResultItem>> {
            override fun onSuccess(value: List<ResultItem>?) {
                val first = value?.firstOrNull()
                if (first != null && first.errorCode != 0) {
                    toast("$tip 失败[${first.errorCode}]：${first.errorMessage.orEmpty()}")
                } else {
                    toast("$tip 成功：${model.identifier}")
                }
            }

            override fun onError(code: Int, desc: String?) {
                toast("$tip 失败[$code]：${desc.orEmpty()}")
            }
        })
    }

    /** 静默批量上报（用于页面初始化时同步），不弹 Toast。 */
    private fun syncReportAllProperties(silent: Boolean) {
        val items = properties.mapNotNull { buildDataItem(it) }
        if (items.isEmpty()) return
        dataModelSession.reportProperty(items, object : TXIoTValueCallback<List<ResultItem>> {
            override fun onSuccess(value: List<ResultItem>?) {
                if (!silent) toast("同步成功")
            }

            override fun onError(code: Int, desc: String?) {
                if (!silent) toast("同步失败[$code]：${desc.orEmpty()}")
            }
        })
    }

    private fun doReportEvent(model: EventModel) {
        if (!isLoggedIn()) {
            toast("请先登录设备")
            return
        }
        val event = Event().apply {
            id = model.identifier
            eventType = model.type
            eventDataList = emptyList()
        }
        dataModelSession.reportEvent(event, object : TXIoTCallback {
            override fun onSuccess() {
                toast("事件上报成功：${model.identifier}")
            }
            override fun onError(errorCode: Int, errorMsg: String?) {
                toast("事件上报失败[$errorCode]：${errorMsg.orEmpty()}")
            }
        })
    }

    /**
     * 云端下发单个属性变更 -> 更新本地缓存并刷新 UI。
     * 注意：此方法一定在主线程执行。
     */
    private fun applyRemotePropertyUpdate(property: Data) {
        val idx = properties.indexOfFirst { it.identifier == property.id }
        if (idx < 0) return
        properties[idx].currentValue = readValue(property)
        refreshList()
    }

    /**
     * 云端下发 Action -> 把 Action 中携带的输入参数同步到本地属性表。
     * 注意：此方法一定在主线程执行。
     */
    private fun handleReceivedAction(action: Action) {
        action.actionInputDataList?.forEach { input ->
            val idx = properties.indexOfFirst { it.identifier == input.id }
            if (idx >= 0) {
                properties[idx].currentValue = readValue(input)
            }
        }
        refreshList()
        toast("收到 Action: ${action.id.orEmpty()}")
    }

    /**
     * 根据当前 UI 中缓存的值构造 SDK 的 Data。
     * 值解析失败返回 null。
     */
    private fun buildDataItem(model: PropertyModel): Data? {
        val value = DataValue()
        val text = model.currentValue
        when (model.type) {
            DataType.STRING -> value.valueString = text
            DataType.INT -> value.valueInt = text.toIntOrNull() ?: return null
            DataType.FLOAT -> value.valueFloat = text.toDoubleOrNull() ?: return null
            DataType.BOOL -> value.valueBool = text.equals("true", ignoreCase = true)
            DataType.ENUM -> value.valueEnum = text.toLongOrNull() ?: return null
            DataType.TIME -> value.valueTime = text.toLongOrNull() ?: return null
        }
        return Data().apply {
            id = model.identifier
            dataType = model.type
            dataValue = value
        }
    }

    private fun readValue(item: Data): String {
        val v = item.dataValue ?: return ""
        val type = item.dataType ?: return ""
        return when (type) {
            DataType.STRING -> v.valueString.orEmpty()
            DataType.INT -> v.valueInt.toString()
            DataType.FLOAT -> v.valueFloat.toString()
            DataType.BOOL -> v.valueBool.toString()
            DataType.ENUM -> v.valueEnum.toString()
            DataType.TIME -> v.valueTime.toString()
        }
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }

    // ==================== 数据模型 ====================

    private data class PropertyModel(
        val identifier: String,
        val displayName: String,
        val type: DataType,
        val readWrite: String,
        val rangeText: String,
        val maxLength: Int,
        var currentValue: String
    )

    private data class EventModel(
        val identifier: String,
        val displayName: String,
        val type: EventType
    )
}
