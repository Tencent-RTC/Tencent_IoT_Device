package com.tencent.liteav.iot.demo

import android.Manifest
import android.animation.ValueAnimator
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.updatePadding
import com.tencent.liteav.iot.TXIoTAITalkSession
import com.tencent.liteav.iot.TXIoTCallback
import com.tencent.liteav.iot.TXIoTError
import com.tencent.liteav.iot.demo.widget.VoiceWaveView

class AITalkActivity : CallAwareActivity() {

    companion object {
        private const val REQ_RECORD_AUDIO = 1001
    }

    private lateinit var etInput: EditText
    private lateinit var ivSend: ImageView
    private lateinit var ivModeSwitch: ImageView
    private lateinit var voiceWave: VoiceWaveView
    private lateinit var llTextInput: View
    private lateinit var scrollView: ScrollView
    private lateinit var llMessages: LinearLayout

    private var textInputMode = false

    private val aiTalkSession: TXIoTAITalkSession by lazy { TXIoTAITalkSession.getInstance() }

    private var currentBotBubble: TextView? = null

    private var thinkingBubble: TextView? = null

    private var thinkingAnimator: ValueAnimator? = null

    private var isSpeaking = false

    private val aiTalkListener = object : TXIoTAITalkSession.Listener {
        override fun onReceiveBotText(text: String?) {
            if (text.isNullOrEmpty()) return
            appendBotText(text)
        }

        override fun onReceiveAsrText(text: String?) {
            if (text.isNullOrBlank()) return
            addUserMessage(text)
        }

        override fun onBotStateChanged(
            oldState: TXIoTAITalkSession.BotState?,
            newState: TXIoTAITalkSession.BotState?
        ) {
            when (newState) {
                TXIoTAITalkSession.BotState.THINKING -> showThinking()
                TXIoTAITalkSession.BotState.LISTENING -> {
                    clearThinking()
                }

                TXIoTAITalkSession.BotState.SPEAKING -> {
                    clearThinking()
                }

                TXIoTAITalkSession.BotState.FINISHED,
                TXIoTAITalkSession.BotState.INTERRUPTED -> {
                    currentBotBubble = null
                    clearThinking()
                }

                else -> Unit
            }
        }

        override fun onLaunchCall(contact: TXIoTAITalkSession.Contact?) {
            toast("收到通话请求：${contact?.userName ?: ""}")
        }

        override fun onError(errorCode: Int, errorMsg: String?) {
            clearThinking()
            currentBotBubble = null
            toast("AI 错误[$errorCode]：${errorMsg.orEmpty()}")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_aitalk)
        setupImmersive()
        bindViews()
        setupHeader()
        setupAITalk()
        setupInput()
    }

    override fun onDestroy() {
        aiTalkSession.removeListener(aiTalkListener)
        stopSpeak()
        super.onDestroy()
    }

    override fun onBeforeAcceptIncomingCall() {
        stopSpeak()
    }

    override fun onAfterAcceptIncomingCall() {
        if (!isFinishing) finish()
    }

    private fun bindViews() {
        etInput = findViewById(R.id.et_input)
        ivSend = findViewById(R.id.iv_send)
        ivModeSwitch = findViewById(R.id.iv_mode_switch)
        voiceWave = findViewById(R.id.voice_wave)
        llTextInput = findViewById(R.id.ll_text_input)
        scrollView = findViewById(R.id.sv_messages)
        llMessages = findViewById(R.id.ll_messages)
    }

    private fun setupImmersive() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = Color.TRANSPARENT
        window.navigationBarColor = Color.TRANSPARENT
        WindowInsetsControllerCompat(window, window.decorView)
            .isAppearanceLightStatusBars = true

        val header = findViewById<View>(R.id.ll_header)
        val headerBasePaddingTop = header.paddingTop
        ViewCompat.setOnApplyWindowInsetsListener(header) { v, insets ->
            val top = insets.getInsets(WindowInsetsCompat.Type.statusBars()).top
            v.updatePadding(top = headerBasePaddingTop + top)
            insets
        }

        val input = findViewById<View>(R.id.ll_input)
        val inputBasePaddingBottom = input.paddingBottom
        ViewCompat.setOnApplyWindowInsetsListener(input) { v, insets ->
            val bottom = insets.getInsets(
                WindowInsetsCompat.Type.navigationBars() or WindowInsetsCompat.Type.ime()
            ).bottom
            v.updatePadding(bottom = inputBasePaddingBottom + bottom)
            insets
        }
    }

    private fun setupHeader() {
    }

    private fun setupInput() {
        etInput.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) =
                Unit

            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(s: Editable?) {
                ivSend.alpha = if (!s.isNullOrBlank()) 1f else 0.6f
            }
        })
        ivSend.alpha = 0.6f

        etInput.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_SEND || actionId == EditorInfo.IME_ACTION_DONE) {
                trySend()
                true
            } else {
                false
            }
        }
        ivSend.setOnClickListener { trySend() }

        ivModeSwitch.setOnClickListener { toggleInputMode() }

        applyInputMode()
    }

    private fun toggleInputMode() {
        textInputMode = !textInputMode
        applyInputMode()
        if (textInputMode) {
            etInput.requestFocus()
        } else {
            etInput.clearFocus()
        }
    }

    private fun applyInputMode() {
        if (textInputMode) {
            voiceWave.visibility = View.GONE
            llTextInput.visibility = View.VISIBLE
            ivModeSwitch.setImageResource(R.drawable.ic_aitalk_mic)
            ivModeSwitch.contentDescription = getString(R.string.aitalk_switch_to_voice)
        } else {
            voiceWave.visibility = View.VISIBLE
            llTextInput.visibility = View.GONE
            ivModeSwitch.setImageResource(R.drawable.ic_aitalk_keyboard)
            ivModeSwitch.contentDescription = getString(R.string.aitalk_switch_to_text)
        }
    }

    private fun setupAITalk() {
        aiTalkSession.addListener(aiTalkListener)
        if (ensureRecordPermission()) {
            startSpeak()
        }
    }

    private fun trySend() {
        val text = etInput.text?.toString()?.trim().orEmpty()
        if (text.isEmpty()) {
            toast("请输入内容")
            return
        }
        addUserMessage(text)
        etInput.setText("")

        val code = aiTalkSession.sendText(text)
        if (code != TXIoTError.SUCCESS) {
            toast("发送失败，错误码：$code")
        } else {
            showThinking()
        }
    }

    private fun startSpeak() {
        if (isSpeaking) return
        val params = TXIoTAITalkSession.Params().apply {
            botId = ""
            promptVariablesJson = ""
        }
        aiTalkSession.startSpeak(params, emptyList(), object : TXIoTCallback {
            override fun onSuccess() {
                isSpeaking = true
                voiceWave.startAnim()
            }

            override fun onError(errorCode: Int, errorMsg: String?) {
                toast("开始说话失败[$errorCode]：${errorMsg.orEmpty()}")
            }
        })
    }

    private fun stopSpeak() {
        if (!isSpeaking) return
        aiTalkSession.stopSpeak()
        isSpeaking = false
        voiceWave.stopAnim()
    }

    private fun addUserMessage(text: String) {
        val view = LayoutInflater.from(this)
            .inflate(R.layout.item_chat_user, llMessages, false)
        view.findViewById<TextView>(R.id.tv_message).text = text
        llMessages.addView(view)
        currentBotBubble = null
        scrollToBottom()
    }

    private fun appendBotText(text: String) {
        clearThinking()
        val bubble = currentBotBubble ?: run {
            val view = LayoutInflater.from(this)
                .inflate(R.layout.item_chat_bot, llMessages, false)
            llMessages.addView(view)
            val tv = view.findViewById<TextView>(R.id.tv_message)
            currentBotBubble = tv
            tv
        }
        bubble.append(text)
        scrollToBottom()
    }

    private fun showThinking() {
        if (thinkingBubble != null) return
        val view = LayoutInflater.from(this)
            .inflate(R.layout.item_chat_bot, llMessages, false)
        val tv = view.findViewById<TextView>(R.id.tv_message)
        tv.text = "."
        thinkingBubble = tv
        llMessages.addView(view)
        startThinkingAnim(tv)
        scrollToBottom()
    }

    private fun startThinkingAnim(tv: TextView) {
        thinkingAnimator?.cancel()
        thinkingAnimator = ValueAnimator.ofInt(0, 3).apply {
            duration = 1200L
            repeatCount = ValueAnimator.INFINITE
            interpolator = android.view.animation.LinearInterpolator()
            addUpdateListener {
                val step = (it.animatedValue as Int) % 3
                tv.text = ".".repeat(step + 1)
            }
            start()
        }
    }

    private fun clearThinking() {
        thinkingAnimator?.cancel()
        thinkingAnimator = null
        val tv = thinkingBubble ?: return
        val root = findItemRoot(tv) ?: return
        llMessages.removeView(root)
        thinkingBubble = null
    }

    private fun findItemRoot(child: View): View? {
        var v: View? = child
        while (v != null && v.parent !== llMessages) {
            v = v.parent as? View
        }
        return v
    }

    private fun scrollToBottom() {
        scrollView.post { scrollView.fullScroll(View.FOCUS_DOWN) }
    }

    private fun ensureRecordPermission(): Boolean {
        val granted = ContextCompat.checkSelfPermission(
            this, Manifest.permission.RECORD_AUDIO
        ) == PackageManager.PERMISSION_GRANTED
        if (!granted) {
            ActivityCompat.requestPermissions(
                this,
                arrayOf(Manifest.permission.RECORD_AUDIO),
                REQ_RECORD_AUDIO
            )
        }
        return granted
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_RECORD_AUDIO) {
            val ok = grantResults.isNotEmpty() &&
                    grantResults[0] == PackageManager.PERMISSION_GRANTED
            if (ok) {
                if (!textInputMode) startSpeak()
            } else {
                toast("需要录音权限才能语音输入")
            }
        }
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}
