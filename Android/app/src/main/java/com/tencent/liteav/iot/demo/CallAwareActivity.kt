package com.tencent.liteav.iot.demo

import androidx.appcompat.app.AppCompatActivity

open class CallAwareActivity : AppCompatActivity() {

    override fun onResume() {
        super.onResume()
        IncomingCallBannerController.attach(this)
    }

    override fun onPause() {
        IncomingCallBannerController.detach(this)
        super.onPause()
    }

    open fun onBeforeAcceptIncomingCall() = Unit

    open fun onAfterAcceptIncomingCall() = Unit
}
