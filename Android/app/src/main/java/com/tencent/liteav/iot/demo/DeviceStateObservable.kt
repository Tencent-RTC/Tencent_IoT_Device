package com.tencent.liteav.iot.demo

import android.widget.TextView
import androidx.annotation.ColorRes
import androidx.annotation.DrawableRes
import androidx.annotation.StringRes
import androidx.core.content.ContextCompat
import com.tencent.liteav.iot.TXIoTDeviceEngine
import java.util.concurrent.CopyOnWriteArrayList

object DeviceStateObservable {

    @Volatile
    var current: TXIoTDeviceEngine.DeviceState = TXIoTDeviceEngine.DeviceState.OFFLINE
        private set

    private val observers = CopyOnWriteArrayList<(TXIoTDeviceEngine.DeviceState) -> Unit>()

    fun addObserver(observer: (TXIoTDeviceEngine.DeviceState) -> Unit) {
        observers.add(observer)
        observer(current)
    }

    fun removeObserver(observer: (TXIoTDeviceEngine.DeviceState) -> Unit) {
        observers.remove(observer)
    }

    internal fun update(newState: TXIoTDeviceEngine.DeviceState) {
        if (newState == current) return
        current = newState
        observers.forEach { it(newState) }
    }
}

data class DeviceStateBadgeStyle(
    @StringRes val textRes: Int,
    @DrawableRes val bgRes: Int,
    @ColorRes val colorRes: Int
)

fun TXIoTDeviceEngine.DeviceState.badgeStyle(): DeviceStateBadgeStyle = when (this) {
    TXIoTDeviceEngine.DeviceState.ONLINE -> DeviceStateBadgeStyle(
        R.string.device_state_online,
        R.drawable.bg_device_state_online,
        R.color.device_state_online_text
    )

    TXIoTDeviceEngine.DeviceState.RECONNECTING -> DeviceStateBadgeStyle(
        R.string.device_state_reconnecting,
        R.drawable.bg_device_state_reconnecting,
        R.color.device_state_reconnecting_text
    )

    TXIoTDeviceEngine.DeviceState.OFFLINE -> DeviceStateBadgeStyle(
        R.string.device_state_offline,
        R.drawable.bg_device_state_offline,
        R.color.device_state_offline_text
    )
}

fun TXIoTDeviceEngine.DeviceState.applyBadge(tv: TextView) {
    val style = badgeStyle()
    tv.setText(style.textRes)
    tv.setBackgroundResource(style.bgRes)
    tv.setTextColor(ContextCompat.getColor(tv.context, style.colorRes))
}
