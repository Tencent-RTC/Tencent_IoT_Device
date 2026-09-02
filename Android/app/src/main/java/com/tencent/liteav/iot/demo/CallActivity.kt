package com.tencent.liteav.iot.demo

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
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
import androidx.core.view.updateLayoutParams
import androidx.core.view.updatePadding
import com.tencent.liteav.iot.TXIoTCallSession
import com.tencent.liteav.iot.TXIoTCallback
import com.tencent.liteav.iot.TXIoTError
import com.tencent.rtmp.ui.TXCloudVideoView
import java.util.Locale

class CallActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_MODE = "extra_mode"
        const val EXTRA_PEER_ID = "extra_peer_id"
        const val EXTRA_PEER_NAME = "extra_peer_name"
        const val EXTRA_MEDIA = "extra_media" // 0=AUDIO 1=VIDEO 2=AUDIO_VIDEO
        const val EXTRA_AUTO_ACCEPTED = "extra_auto_accepted"
        const val MODE_OUTGOING = 0
        const val MODE_INCOMING = 1

        private const val REQ_AV_PERM = 3001
        private const val TAG = "CallActivity"
    }

    private lateinit var tvState: TextView
    private lateinit var tvPeerName: TextView
    private lateinit var tvPeerId: TextView
    private lateinit var tvDuration: TextView
    private lateinit var tvStateTalking: TextView
    private lateinit var tvDurationTalking: TextView
    private lateinit var llIncoming: LinearLayout
    private lateinit var llOutgoing: LinearLayout
    private lateinit var ivMute: ImageView
    private lateinit var ivSpeaker: ImageView
    private lateinit var ivCamera: ImageView
    private lateinit var tvMuteLabel: TextView
    private lateinit var tvSpeakerLabel: TextView
    private lateinit var tvCameraLabel: TextView
    private lateinit var btnSwitchCamera: View
    private lateinit var flAvatar: View
    private lateinit var videoRemote: TXCloudVideoView
    private lateinit var videoLocal: TXCloudVideoView

    private val handler = Handler(Looper.getMainLooper())
    private var callStartTime = 0L

    private var isMuted = false
    private var isSpeakerOn = true
    private var isCameraOn = true
    private var isFrontCamera = true
    private var isTalking = false
    private var isCameraOpened = false
    private var isMicOpened = false
    private var isRemoteViewStarted = false

    private var mode: Int = MODE_OUTGOING
    private var peerId: String = ""
    private var peerName: String = ""
    private var autoAccepted: Boolean = false
    private var mediaContent: TXIoTCallSession.MediaContent =
        TXIoTCallSession.MediaContent.AUDIO_VIDEO

    private val callSession: TXIoTCallSession by lazy { TXIoTCallSession.getInstance() }

    private val callListener = object : TXIoTCallSession.Listener {
        override fun onCallRequested(
            contact: TXIoTCallSession.Contact?,
            option: TXIoTCallSession.Option?
        ) {
            val id = contact?.userId
            Log.i(TAG, "[listener] onCallRequested contact.userId=$id, current peerId=$peerId, media=${option?.mediaContent}")
            if (id.isNullOrEmpty()) return
            if (peerId.isEmpty()) {
                peerId = id
                peerName = contact.userName.orEmpty()
                option?.mediaContent?.let { mediaContent = it }
                bindPeerInfo()
            }
        }

        override fun onCallAccepted(contact: TXIoTCallSession.Contact?) {
            Log.i(TAG, "[listener] onCallAccepted contact.userId=${contact?.userId}, current peerId=$peerId")
            startTalking()
        }

        override fun onCallRejected(contact: TXIoTCallSession.Contact?) {
            Log.i(TAG, "[listener] onCallRejected contact.userId=${contact?.userId}, current peerId=$peerId")
            tvState.text = "对方已拒接"
            finishLater()
        }

        override fun onCallTimeout(contact: TXIoTCallSession.Contact?) {
            Log.i(TAG, "[listener] onCallTimeout contact.userId=${contact?.userId}, current peerId=$peerId")
            tvState.text = "无人接听"
            finishLater()
        }

        override fun onCallHangup(contact: TXIoTCallSession.Contact?) {
            Log.i(TAG, "[listener] onCallHangup contact.userId=${contact?.userId}, current peerId=$peerId, isTalking=$isTalking")
            hangupInternal(notifySdk = false)
        }
    }

    private val durationTicker = object : Runnable {
        override fun run() {
            val elapsed = (SystemClock.elapsedRealtime() - callStartTime) / 1000
            val text = String.format(
                Locale.US, "%02d:%02d", elapsed / 60, elapsed % 60
            )
            tvDuration.text = text
            tvDurationTalking.text = text
            handler.postDelayed(this, 1000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_call)
        setupImmersiveStatusBar()

        bindViews()
        parseIntent()
        bindClicks()
        applySystemBarInsets()

        callSession.addListener(callListener)
        Log.i(TAG, "onCreate: addListener callListener=$callListener, mode=$mode, peerId=$peerId, media=$mediaContent")

        bindPeerInfo()
        setupModeUi()
    }

    // ==================== 沉浸式状态栏 ====================

    private fun setupImmersiveStatusBar() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.statusBarColor = Color.TRANSPARENT
        window.navigationBarColor = Color.TRANSPARENT
        WindowInsetsControllerCompat(window, window.decorView)
            .isAppearanceLightStatusBars = false
    }

    private fun applySystemBarInsets() {
        val stateBaseMarginTop = (tvState.layoutParams as? ViewGroup.MarginLayoutParams)?.topMargin ?: 0
        ViewCompat.setOnApplyWindowInsetsListener(tvState) { v, insets ->
            val top = insets.getInsets(WindowInsetsCompat.Type.statusBars()).top
            v.updateLayoutParams<ViewGroup.MarginLayoutParams> {
                topMargin = stateBaseMarginTop + top
            }
            insets
        }

        val incomingBaseBottom = (llIncoming.layoutParams as? ViewGroup.MarginLayoutParams)?.bottomMargin ?: 0
        ViewCompat.setOnApplyWindowInsetsListener(llIncoming) { v, insets ->
            val bottom = insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom
            v.updateLayoutParams<ViewGroup.MarginLayoutParams> {
                bottomMargin = incomingBaseBottom + bottom
            }
            insets
        }
        val outgoingBaseBottom = (llOutgoing.layoutParams as? ViewGroup.MarginLayoutParams)?.bottomMargin ?: 0
        ViewCompat.setOnApplyWindowInsetsListener(llOutgoing) { v, insets ->
            val bottom = insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom
            v.updateLayoutParams<ViewGroup.MarginLayoutParams> {
                bottomMargin = outgoingBaseBottom + bottom
            }
            insets
        }

        ViewCompat.requestApplyInsets(window.decorView)
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        if (isTalking && peerId.isNotEmpty()) {
            callSession.hangup(peerId, noopCallback())
        }
        if (isCameraOpened) callSession.closeCamera()
        if (isMicOpened) callSession.closeMicrophone()
        if (isRemoteViewStarted && peerId.isNotEmpty()) callSession.stopRemoteView(peerId)
        callSession.removeListener(callListener)
        super.onDestroy()
    }

    private fun bindViews() {
        tvState = findViewById(R.id.tv_call_state)
        tvPeerName = findViewById(R.id.tv_call_peer_name)
        tvPeerId = findViewById(R.id.tv_call_peer_id)
        tvDuration = findViewById(R.id.tv_call_duration)
        tvStateTalking = findViewById(R.id.tv_call_state_talking)
        tvDurationTalking = findViewById(R.id.tv_call_duration_talking)
        llIncoming = findViewById(R.id.ll_call_incoming_actions)
        llOutgoing = findViewById(R.id.ll_call_outgoing_actions)
        ivMute = findViewById(R.id.iv_call_mute)
        ivSpeaker = findViewById(R.id.iv_call_speaker)
        ivCamera = findViewById(R.id.iv_call_camera)
        tvMuteLabel = findViewById(R.id.tv_call_mute_label)
        tvSpeakerLabel = findViewById(R.id.tv_call_speaker_label)
        tvCameraLabel = findViewById(R.id.tv_call_camera_label)
        btnSwitchCamera = findViewById(R.id.btn_call_switch_camera)
        flAvatar = findViewById(R.id.fl_call_avatar)
        videoRemote = findViewById(R.id.video_call_remote)
        videoLocal = findViewById(R.id.video_call_local)
    }

    private fun parseIntent() {
        mode = intent?.getIntExtra(EXTRA_MODE, MODE_OUTGOING) ?: MODE_OUTGOING
        peerId = intent?.getStringExtra(EXTRA_PEER_ID).orEmpty()
        peerName = intent?.getStringExtra(EXTRA_PEER_NAME).orEmpty()
        autoAccepted = intent?.getBooleanExtra(EXTRA_AUTO_ACCEPTED, false) ?: false
        mediaContent = when (intent?.getIntExtra(EXTRA_MEDIA, 2) ?: 2) {
            0 -> TXIoTCallSession.MediaContent.AUDIO
            1 -> TXIoTCallSession.MediaContent.VIDEO
            else -> TXIoTCallSession.MediaContent.AUDIO_VIDEO
        }
    }

    private fun bindClicks() {
        findViewById<View>(R.id.btn_call_accept).setOnClickListener { onClickAccept() }
        findViewById<View>(R.id.btn_call_reject).setOnClickListener { onClickReject() }
        findViewById<View>(R.id.btn_call_mute).setOnClickListener { toggleMute() }
        findViewById<View>(R.id.btn_call_speaker).setOnClickListener { toggleSpeaker() }
        findViewById<View>(R.id.btn_call_camera).setOnClickListener { toggleCamera() }
        findViewById<View>(R.id.btn_call_hangup).setOnClickListener { hangupInternal(true) }
        btnSwitchCamera.setOnClickListener { switchCamera() }
    }

    private fun bindPeerInfo() {
        tvPeerName.visibility = View.GONE
        tvPeerId.text = if (peerId.isNotEmpty()) "UserId：$peerId" else "UserId：--"
    }

    private fun setupModeUi() {
        when (mode) {
            MODE_INCOMING -> {
                if (autoAccepted) {
                    llIncoming.visibility = View.GONE
                    llOutgoing.visibility = View.VISIBLE
                    if (ensureAvPermission()) {
                        startTalking()
                    }
                    return
                }
                tvState.text = getString(R.string.call_state_incoming)
                llIncoming.visibility = View.VISIBLE
                llOutgoing.visibility = View.GONE
                if (mediaContent != TXIoTCallSession.MediaContent.AUDIO && ensureAvPermission()) {
                    startIncomingPreview()
                }
            }
            else -> {
                tvState.text = getString(R.string.call_state_calling)
                llIncoming.visibility = View.GONE
                llOutgoing.visibility = View.VISIBLE
                if (peerId.isEmpty()) {
                    toast("缺少对端 userId，无法呼叫")
                    finishLater(300L)
                    return
                }
                if (ensureAvPermission()) doOutgoingCall()
            }
        }
    }

    private fun startIncomingPreview() {
        if (isCameraOpened) return
        applyLocalVideoFullscreen()
        videoLocal.visibility = View.VISIBLE
        videoLocal.post {
            if (isFinishing || isDestroyed) return@post
            val ret = callSession.openCamera(true, videoLocal)
            if (ret == TXIoTError.SUCCESS) isCameraOpened = true
        }
    }

    private fun applyLocalVideoFullscreen() {
        val root = videoLocal.parent as? ConstraintLayout ?: return
        val id = videoLocal.id
        val set = ConstraintSet().apply { clone(root) }
        set.constrainWidth(id, 0)
        set.constrainHeight(id, 0)
        set.connect(id, ConstraintSet.TOP, ConstraintSet.PARENT_ID, ConstraintSet.TOP, 0)
        set.connect(id, ConstraintSet.BOTTOM, ConstraintSet.PARENT_ID, ConstraintSet.BOTTOM, 0)
        set.connect(id, ConstraintSet.START, ConstraintSet.PARENT_ID, ConstraintSet.START, 0)
        set.connect(id, ConstraintSet.END, ConstraintSet.PARENT_ID, ConstraintSet.END, 0)
        set.applyTo(root)
        ViewCompat.requestApplyInsets(root)
    }

    private fun applyLocalVideoCorner() {
        val root = videoLocal.parent as? ConstraintLayout ?: return
        val id = videoLocal.id
        val density = resources.displayMetrics.density
        val w = (108 * density).toInt()
        val h = (160 * density).toInt()
        val margin = (16 * density).toInt()
        val topMargin = (72 * density).toInt()
        val set = ConstraintSet().apply { clone(root) }
        set.constrainWidth(id, w)
        set.constrainHeight(id, h)
        set.clear(id, ConstraintSet.BOTTOM)
        set.clear(id, ConstraintSet.START)
        set.connect(id, ConstraintSet.TOP, ConstraintSet.PARENT_ID, ConstraintSet.TOP, topMargin)
        set.connect(id, ConstraintSet.END, ConstraintSet.PARENT_ID, ConstraintSet.END, margin)
        set.applyTo(root)
        ViewCompat.requestApplyInsets(root)
    }

    private fun ensureAvPermission(): Boolean {
        val perms = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (mediaContent != TXIoTCallSession.MediaContent.AUDIO) {
            perms += Manifest.permission.CAMERA
        }
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
        if (!allGranted) {
            toast("需要相机与麦克风权限")
            finishLater(300L)
            return
        }
        when (mode) {
            MODE_OUTGOING -> doOutgoingCall()
            MODE_INCOMING -> {
                if (autoAccepted && !isTalking) {
                    startTalking()
                } else if (!isTalking && mediaContent != TXIoTCallSession.MediaContent.AUDIO) {
                    startIncomingPreview()
                }
            }
        }
    }

    // ==================== 呼出 ====================

    private fun doOutgoingCall() {
        val option = TXIoTCallSession.Option().apply {
            mediaContent = this@CallActivity.mediaContent
            customData = ""
        }
        callSession.call(peerId, option, object : TXIoTCallback {
            override fun onSuccess() {
                tvState.text = getString(R.string.call_state_calling)
            }

            override fun onError(code: Int, desc: String?) {
                toast("呼叫失败[$code]：${desc.orEmpty()}")
                finishLater()
            }
        })
    }

    // ==================== 呼入接听/拒接 ====================

    private fun onClickAccept() {
        if (peerId.isEmpty()) {
            toast("对端未知")
            return
        }
        if (!ensureAvPermission()) return
        doAccept()
    }

    private fun doAccept() {
        callSession.accept(peerId, object : TXIoTCallback {
            override fun onSuccess() {
                startTalking()
            }

            override fun onError(code: Int, desc: String?) {
                toast("接听失败[$code]：${desc.orEmpty()}")
            }
        })
    }

    private fun onClickReject() {
        if (peerId.isEmpty()) {
            finish()
            return
        }
        callSession.reject(peerId, object : TXIoTCallback {
            override fun onSuccess() {
                toast("已拒接")
                finishLater(200L)
            }

            override fun onError(code: Int, desc: String?) {
                toast("拒接失败[$code]：${desc.orEmpty()}")
                finish()
            }
        })
    }

    // ==================== 通话中 ====================

    private fun startTalking() {
        if (isTalking) return
        isTalking = true
        tvState.text = getString(R.string.call_state_talking)
        llIncoming.visibility = View.GONE
        llOutgoing.visibility = View.VISIBLE
        tvDuration.visibility = View.GONE
        tvStateTalking.visibility = View.VISIBLE
        tvDurationTalking.visibility = View.VISIBLE
        tvState.visibility = View.INVISIBLE
        flAvatar.visibility = View.INVISIBLE
        tvPeerId.visibility = View.INVISIBLE

        val micRet = callSession.openMicrophone()
        if (micRet == TXIoTError.SUCCESS) isMicOpened = true
        isMuted = false
        refreshMuteUi()
        isSpeakerOn = true
        refreshSpeakerUi()

        if (mediaContent != TXIoTCallSession.MediaContent.AUDIO) {
            applyLocalVideoCorner()
            videoRemote.visibility = View.VISIBLE
            videoLocal.visibility = View.VISIBLE
            btnSwitchCamera.visibility = View.VISIBLE
            isCameraOn = true
            refreshCameraUi()

            // 关键修复：applyLocalVideoCorner 触发的 SurfaceView 尺寸切换会让 local SurfaceView 重建 Surface，
            // 若此时并发对 videoRemote 调用 startRemoteView，两个 SurfaceView 的 Surface 就绪时序不确定，
            // 会偶发出现远端首帧渲染丢失，屏幕上只剩下默认头像。延迟一小段时间让 local 布局稳定后再拉流。
            handler.postDelayed({
                if (isFinishing || isDestroyed || !isTalking) return@postDelayed
                if (peerId.isNotEmpty() && !isRemoteViewStarted) {
                    callSession.startRemoteView(peerId, videoRemote)
                    isRemoteViewStarted = true
                }
            }, 100L)

            if (!isCameraOpened) {
                videoLocal.post {
                    if (isFinishing || isDestroyed || !isTalking) return@post
                    val ret = callSession.openCamera(true, videoLocal)
                    if (ret == TXIoTError.SUCCESS) isCameraOpened = true
                }
            }
        }

        callStartTime = SystemClock.elapsedRealtime()
        handler.post(durationTicker)
    }

    private fun toggleMute() {
        isMuted = !isMuted
        if (isMuted) {
            callSession.closeMicrophone()
            isMicOpened = false
        } else {
            val ret = callSession.openMicrophone()
            if (ret == TXIoTError.SUCCESS) isMicOpened = true
        }
        refreshMuteUi()
    }

    private fun refreshMuteUi() {
        if (isMuted) {
            ivMute.setBackgroundResource(R.drawable.bg_call_action_dark)
            ivMute.setImageResource(R.drawable.ic_call_mic_off)
            ivMute.imageTintList = android.content.res.ColorStateList.valueOf(0xFFFFFFFF.toInt())
        } else {
            ivMute.setBackgroundResource(R.drawable.bg_call_action_white)
            ivMute.setImageResource(R.drawable.ic_aitalk_mic)
            ivMute.imageTintList = android.content.res.ColorStateList.valueOf(0xFF1A1A1A.toInt())
        }
        ivMute.alpha = 1f
        tvMuteLabel.setText(
            if (isMuted) R.string.call_action_mic_off else R.string.call_action_mic_on
        )
    }

    private fun toggleSpeaker() {
        if (peerId.isEmpty()) return
        isSpeakerOn = !isSpeakerOn
        callSession.muteRemoteAudio(peerId, !isSpeakerOn)
        refreshSpeakerUi()
    }

    private fun refreshSpeakerUi() {
        if (isSpeakerOn) {
            ivSpeaker.setBackgroundResource(R.drawable.bg_call_action_white)
            ivSpeaker.setImageResource(R.drawable.ic_call_speaker)
            ivSpeaker.imageTintList = android.content.res.ColorStateList.valueOf(0xFF1A1A1A.toInt())
        } else {
            ivSpeaker.setBackgroundResource(R.drawable.bg_call_action_dark)
            ivSpeaker.setImageResource(R.drawable.ic_call_speaker_off)
            ivSpeaker.imageTintList = android.content.res.ColorStateList.valueOf(0xFFFFFFFF.toInt())
        }
        ivSpeaker.alpha = 1f
        tvSpeakerLabel.setText(
            if (isSpeakerOn) R.string.call_action_speaker_on else R.string.call_action_speaker_off
        )
    }

    private fun toggleCamera() {
        if (mediaContent == TXIoTCallSession.MediaContent.AUDIO) {
            toast("当前为语音通话，不支持摄像头")
            return
        }
        isCameraOn = !isCameraOn
        if (isCameraOn) {
            if (!isCameraOpened) {
                videoLocal.post {
                    if (isFinishing || isDestroyed) return@post
                    val ret = callSession.openCamera(isFrontCamera, videoLocal)
                    if (ret == TXIoTError.SUCCESS) isCameraOpened = true
                }
            }
            videoLocal.visibility = View.VISIBLE
            btnSwitchCamera.visibility = View.VISIBLE
        } else {
            if (isCameraOpened) {
                callSession.closeCamera()
                isCameraOpened = false
            }
            videoLocal.visibility = View.INVISIBLE
            btnSwitchCamera.visibility = View.INVISIBLE
        }
        refreshCameraUi()
    }

    private fun refreshCameraUi() {
        if (isCameraOn) {
            ivCamera.setBackgroundResource(R.drawable.bg_call_action_white)
            ivCamera.setImageResource(R.drawable.ic_call_video)
            ivCamera.imageTintList = android.content.res.ColorStateList.valueOf(0xFF1A1A1A.toInt())
        } else {
            ivCamera.setBackgroundResource(R.drawable.bg_call_action_dark)
            ivCamera.setImageResource(R.drawable.ic_call_video_off)
            ivCamera.imageTintList = android.content.res.ColorStateList.valueOf(0xFFFFFFFF.toInt())
        }
        ivCamera.alpha = 1f
        tvCameraLabel.setText(
            if (isCameraOn) R.string.call_action_camera_on else R.string.call_action_camera_off
        )
    }

    private fun switchCamera() {
        if (!isCameraOpened) {
            toast("摄像头未开启")
            return
        }
        isFrontCamera = !isFrontCamera
        callSession.switchCamera(isFrontCamera)
    }

    private fun hangupInternal(notifySdk: Boolean) {
        handler.removeCallbacks(durationTicker)
        if (notifySdk && peerId.isNotEmpty()) {
            callSession.hangup(peerId, noopCallback())
        }
        if (isMicOpened) {
            callSession.closeMicrophone()
            isMicOpened = false
        }
        if (isCameraOpened) {
            callSession.closeCamera()
            isCameraOpened = false
        }
        if (isRemoteViewStarted && peerId.isNotEmpty()) {
            callSession.stopRemoteView(peerId)
            isRemoteViewStarted = false
        }
        tvStateTalking.visibility = View.GONE
        tvDurationTalking.visibility = View.GONE
        btnSwitchCamera.visibility = View.GONE
        tvState.visibility = View.VISIBLE
        tvState.text = getString(R.string.call_state_ended)
        toast(if (notifySdk) "已挂断" else "对方已挂断")
        isTalking = false
        Log.i(TAG, "hangupInternal: scheduled finishLater")
        finishLater()
    }

    // ==================== 工具方法 ====================

    private fun finishLater(delayMillis: Long = 500L) {
        handler.postDelayed({
            if (!isFinishing) finish()
        }, delayMillis)
    }

    private fun noopCallback(): TXIoTCallback = object : TXIoTCallback {
        override fun onSuccess() = Unit
        override fun onError(code: Int, desc: String?) = Unit
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}