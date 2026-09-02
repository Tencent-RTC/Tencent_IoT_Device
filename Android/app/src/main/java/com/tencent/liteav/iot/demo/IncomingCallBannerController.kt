package com.tencent.liteav.iot.demo

import android.app.Activity
import android.content.Intent
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import com.tencent.liteav.iot.TXIoTCallSession
import com.tencent.liteav.iot.TXIoTCallback

object IncomingCallBannerController {

    private const val TAG = "IncomingCallBanner"

    private var installed = false

    private var currentPeerId: String = ""
    private var currentPeerName: String = ""
    private var currentMedia: TXIoTCallSession.MediaContent =
        TXIoTCallSession.MediaContent.AUDIO_VIDEO

    private var currentHost: CallAwareActivity? = null

    private var currentBanner: View? = null

    private val callSession: TXIoTCallSession by lazy { TXIoTCallSession.getInstance() }

    private val globalListener = object : TXIoTCallSession.Listener {
        override fun onCallRequested(
            contact: TXIoTCallSession.Contact?,
            option: TXIoTCallSession.Option?
        ) {
            val id = contact?.userId ?: return
            if (id.isEmpty()) return
            currentPeerId = id
            currentPeerName = contact.userName.orEmpty()
            currentMedia = option?.mediaContent
                ?: TXIoTCallSession.MediaContent.AUDIO_VIDEO
            Log.i(TAG, "onCallRequested peer=$id host=${currentHost?.javaClass?.simpleName}")
            currentHost?.let { showBannerOn(it) } ?: run {
                fallbackLaunchIncomingActivity()
            }
        }

        override fun onCallRejected(contact: TXIoTCallSession.Contact?) {
            Log.i(TAG, "onCallRejected peer=${contact?.userId}")
            clear()
        }

        override fun onCallHangup(contact: TXIoTCallSession.Contact?) {
            Log.i(TAG, "onCallHangup peer=${contact?.userId}")
            clear()
        }

        override fun onCallTimeout(contact: TXIoTCallSession.Contact?) {
            Log.i(TAG, "onCallTimeout peer=${contact?.userId}")
            clear()
        }
    }

    fun install() {
        if (installed) return
        installed = true
        callSession.addListener(globalListener)
    }

    fun attach(activity: CallAwareActivity) {
        currentHost = activity
        if (currentPeerId.isNotEmpty()) {
            showBannerOn(activity)
        }
    }

    fun detach(activity: CallAwareActivity) {
        if (currentHost === activity) {
            removeBannerFromHost()
            currentHost = null
        }
    }

    private fun clear() {
        currentPeerId = ""
        currentPeerName = ""
        currentMedia = TXIoTCallSession.MediaContent.AUDIO_VIDEO
        removeBannerFromHost()
    }

    private fun buildTitleText(peerName: String, peerId: String): String {
        val name = peerName.trim()
        val id = peerId.trim()
        return when {
            name.isNotEmpty() && id.isNotEmpty() -> "$name ($id)"
            name.isNotEmpty() -> name
            id.isNotEmpty() -> "来电 ($id)"
            else -> "来电"
        }
    }

    private fun removeBannerFromHost() {
        val banner = currentBanner ?: return
        (banner.parent as? ViewGroup)?.removeView(banner)
        currentBanner = null
    }

    private fun showBannerOn(activity: CallAwareActivity) {
        removeBannerFromHost()
        val decor = activity.window.decorView as? ViewGroup ?: return
        val inflater = activity.layoutInflater
        val banner = inflater.inflate(R.layout.view_incoming_call_banner, decor, false)

        val title = banner.findViewById<TextView>(R.id.tv_incoming_call_title)
        val desc = banner.findViewById<TextView>(R.id.tv_incoming_call_desc)
        title.text = buildTitleText(currentPeerName, currentPeerId)
        desc.text = when (currentMedia) {
            TXIoTCallSession.MediaContent.AUDIO -> "邀请你语音通话..."
            TXIoTCallSession.MediaContent.VIDEO -> "邀请你视频通话..."
            TXIoTCallSession.MediaContent.AUDIO_VIDEO -> "邀请你视频通话..."
            else -> "邀请你视频通话..."
        }

        banner.findViewById<View>(R.id.btn_incoming_call_accept).setOnClickListener {
            onAcceptClicked(activity)
        }
        banner.findViewById<View>(R.id.btn_incoming_call_reject).setOnClickListener {
            onRejectClicked(activity)
        }
        banner.setOnClickListener {
            onBlankClicked(activity)
        }

        val density = activity.resources.displayMetrics.density
        val marginH = (10 * density).toInt()
        val targetTopFromScreen = (74 * density).toInt()

        val lp = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        ).apply {
            leftMargin = marginH
            rightMargin = marginH
            topMargin = targetTopFromScreen
            gravity = android.view.Gravity.TOP
        }
        banner.layoutParams = lp
        banner.elevation = 24 * density

        decor.addView(banner)
        currentBanner = banner

        banner.alpha = 0f
        banner.translationY = -20 * density
        banner.animate().alpha(1f).translationY(0f).setDuration(220L).start()
    }

    private fun onAcceptClicked(activity: CallAwareActivity) {
        val peerId = currentPeerId
        val peerName = currentPeerName
        val media = currentMedia
        if (peerId.isEmpty()) return

        activity.onBeforeAcceptIncomingCall()

        callSession.accept(peerId, object : TXIoTCallback {
            override fun onSuccess() {
                clear()
                launchCallActivity(activity, peerId, peerName, media, autoAccepted = true)
                activity.onAfterAcceptIncomingCall()
            }

            override fun onError(code: Int, desc: String?) {
                Toast.makeText(
                    activity, "接听失败[$code]：${desc.orEmpty()}", Toast.LENGTH_SHORT
                ).show()
            }
        })
    }

    private fun onRejectClicked(activity: CallAwareActivity) {
        val peerId = currentPeerId
        if (peerId.isEmpty()) return
        callSession.reject(peerId, object : TXIoTCallback {
            override fun onSuccess() {
                clear()
            }
            override fun onError(code: Int, desc: String?) {
                Toast.makeText(
                    activity, "拒接失败[$code]：${desc.orEmpty()}", Toast.LENGTH_SHORT
                ).show()
                clear()
            }
        })
    }

    private fun onBlankClicked(activity: CallAwareActivity) {
        val peerId = currentPeerId
        if (peerId.isEmpty()) return
        val peerName = currentPeerName
        val media = currentMedia
        activity.onBeforeAcceptIncomingCall()
        clear()
        launchCallActivity(activity, peerId, peerName, media, autoAccepted = false)
        activity.onAfterAcceptIncomingCall()
    }

    private fun fallbackLaunchIncomingActivity() {
        val ctx = currentHost ?: return
        val peerId = currentPeerId
        if (peerId.isEmpty()) return
        val peerName = currentPeerName
        val media = currentMedia
        clear()
        launchCallActivity(ctx, peerId, peerName, media, autoAccepted = false)
    }

    private fun launchCallActivity(
        activity: Activity,
        peerId: String,
        peerName: String,
        media: TXIoTCallSession.MediaContent,
        autoAccepted: Boolean
    ) {
        val mediaFlag = when (media) {
            TXIoTCallSession.MediaContent.AUDIO -> 0
            TXIoTCallSession.MediaContent.VIDEO -> 1
            else -> 2
        }
        val intent = Intent(activity, CallActivity::class.java).apply {
            putExtra(CallActivity.EXTRA_MODE, CallActivity.MODE_INCOMING)
            putExtra(CallActivity.EXTRA_PEER_ID, peerId)
            putExtra(CallActivity.EXTRA_PEER_NAME, peerName)
            putExtra(CallActivity.EXTRA_MEDIA, mediaFlag)
            putExtra(CallActivity.EXTRA_AUTO_ACCEPTED, autoAccepted)
            addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
        }
        activity.startActivity(intent)
    }
}
