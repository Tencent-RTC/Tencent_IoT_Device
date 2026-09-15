package com.tencent.liteav.iot.demo

import android.content.Context
import android.view.LayoutInflater
import android.view.View
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.tencent.liteav.iot.demo.util.LocalContactStore

/**
 * 手动添加联系人的底部弹窗，支持两种类型：
 * - VoIP 用户: userId = modelId/appId/openId
 * - 物联网设备: userId = productId/deviceName
 */
class AddContactDialog(
    private val context: Context,
    private val onAdded: (LocalContactStore.LocalContact) -> Unit
) {

    private var currentType: Int = LocalContactStore.TYPE_VOIP

    fun show() {
        val dialog = BottomSheetDialog(
            context,
            com.google.android.material.R.style.Theme_Design_Light_BottomSheetDialog
        )
        val view = LayoutInflater.from(context)
            .inflate(R.layout.dialog_add_contact, null, false)

        val tvVoip = view.findViewById<TextView>(R.id.tv_add_contact_type_voip)
        val tvIot = view.findViewById<TextView>(R.id.tv_add_contact_type_iot)
        val tvDesc = view.findViewById<TextView>(R.id.tv_add_contact_type_desc)
        val etUserId = view.findViewById<EditText>(R.id.et_add_contact_userid)
        val etName = view.findViewById<EditText>(R.id.et_add_contact_name)
        val tvCancel = view.findViewById<TextView>(R.id.tv_add_contact_cancel)
        val tvConfirm = view.findViewById<TextView>(R.id.tv_add_contact_confirm)

        fun applyType(type: Int) {
            currentType = type
            val isVoip = type == LocalContactStore.TYPE_VOIP
            tvVoip.isSelected = isVoip
            tvIot.isSelected = !isVoip
            tvDesc.setText(
                if (isVoip) R.string.add_contact_voip_desc
                else R.string.add_contact_iot_desc
            )
            etUserId.setHint(
                if (isVoip) R.string.add_contact_voip_hint
                else R.string.add_contact_iot_hint
            )
        }
        applyType(LocalContactStore.TYPE_VOIP)

        tvVoip.setOnClickListener { applyType(LocalContactStore.TYPE_VOIP) }
        tvIot.setOnClickListener { applyType(LocalContactStore.TYPE_IOT) }

        tvCancel.setOnClickListener { dialog.dismiss() }
        tvConfirm.setOnClickListener {
            val userId = etUserId.text?.toString()?.trim().orEmpty()
            val name = etName.text?.toString()?.trim().orEmpty()

            if (userId.isEmpty()) {
                toast(R.string.add_contact_empty_userid)
                return@setOnClickListener
            }
            val parts = userId.split("/")
            when (currentType) {
                LocalContactStore.TYPE_VOIP -> {
                    if (parts.size != 3 || parts.any { it.isEmpty() }) {
                        toast(R.string.add_contact_invalid_voip)
                        return@setOnClickListener
                    }
                }
                LocalContactStore.TYPE_IOT -> {
                    if (parts.size != 2 || parts.any { it.isEmpty() }) {
                        toast(R.string.add_contact_invalid_iot)
                        return@setOnClickListener
                    }
                }
            }

            val contact = LocalContactStore.LocalContact(
                userId = userId,
                userName = name,
                type = currentType
            )
            val ok = LocalContactStore.add(context, contact)
            if (!ok) {
                toast(R.string.add_contact_duplicated)
                return@setOnClickListener
            }
            toast(R.string.add_contact_success)
            onAdded(contact)
            dialog.dismiss()
        }

        dialog.setContentView(view)
        (view.parent as? View)?.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        dialog.show()
    }

    private fun toast(resId: Int) {
        Toast.makeText(context, context.getString(resId), Toast.LENGTH_SHORT).show()
    }
}
