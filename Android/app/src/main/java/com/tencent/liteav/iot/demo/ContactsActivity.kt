package com.tencent.liteav.iot.demo

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.tencent.liteav.iot.TXIoTCallSession
import com.tencent.liteav.iot.TXIoTValueCallback
import com.tencent.liteav.iot.demo.util.LocalContactStore
import com.tencent.liteav.iot.demo.widget.SwipeRevealLayout

class ContactsActivity : CallAwareActivity() {

    private lateinit var recyclerView: RecyclerView
    private lateinit var loadingContainer: View
    private lateinit var emptyView: View

    private val callSession: TXIoTCallSession by lazy { TXIoTCallSession.getInstance() }
    private val contacts = mutableListOf<ContactEntry>()
    private val adapter = ContactAdapter()

    private var openedSwipe: SwipeRevealLayout? = null
    private var nextCursor: String = ""
    private var hasMore: Boolean = true
    private var isLoading: Boolean = false
    private var showFooter: Boolean = false
    private var footerLoading: Boolean = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_contacts)

        recyclerView = findViewById(R.id.rv_contacts)
        loadingContainer = findViewById(R.id.ll_contacts_loading)
        emptyView = findViewById(R.id.tv_contacts_empty)

        findViewById<View>(R.id.iv_contacts_back).setOnClickListener { finish() }
        findViewById<View>(R.id.iv_contacts_add).setOnClickListener {
            AddContactDialog(this) { local -> onLocalContactAdded(local) }.show()
        }

        recyclerView.layoutManager = LinearLayoutManager(this)
        recyclerView.adapter = adapter
        recyclerView.addOnScrollListener(object : RecyclerView.OnScrollListener() {
            override fun onScrolled(rv: RecyclerView, dx: Int, dy: Int) {
                if (dy <= 0) return
                val lm = rv.layoutManager as? LinearLayoutManager ?: return
                val last = lm.findLastVisibleItemPosition()
                val total = adapter.itemCount
                if (total > 0 && last >= total - 1) {
                    loadNextPage()
                }
            }
        })

        loadFirstPage()
    }

    private fun loadFirstPage() {
        contacts.clear()
        // 先加载本地手动添加的联系人
        LocalContactStore.getAll(this).forEach {
            contacts.add(
                ContactEntry(
                    LocalContactStore.toSessionContact(it),
                    it.type,
                    isLocal = true
                )
            )
        }
        showFooter = false
        footerLoading = false
        adapter.notifyDataSetChanged()
        nextCursor = ""
        hasMore = true
        loadingContainer.visibility = View.VISIBLE
        emptyView.visibility = View.GONE
        recyclerView.visibility = View.GONE
        fetchPage(isFirst = true)
    }

    private fun onLocalContactAdded(local: LocalContactStore.LocalContact) {
        contacts.add(
            0,
            ContactEntry(
                LocalContactStore.toSessionContact(local),
                local.type,
                isLocal = true
            )
        )
        loadingContainer.visibility = View.GONE
        emptyView.visibility = View.GONE
        recyclerView.visibility = View.VISIBLE
        adapter.notifyDataSetChanged()
    }

    private fun onDeleteLocalContact(position: Int, entry: ContactEntry) {
        if (!entry.isLocal) return
        val userId = entry.contact.userId.orEmpty()
        LocalContactStore.remove(this, userId)
        contacts.removeAt(position)
        openedSwipe = null
        adapter.notifyItemRemoved(position)
        if (contacts.isEmpty() && !isLoading) {
            emptyView.visibility = View.VISIBLE
            recyclerView.visibility = View.GONE
        }
        Toast.makeText(this, R.string.contacts_delete_success, Toast.LENGTH_SHORT).show()
    }

    private fun loadNextPage() {
        if (isLoading || !hasMore) return
        footerLoading = true
        showFooter = true
        adapter.notifyItemChanged(adapter.itemCount - 1)
        fetchPage(isFirst = false)
    }

    private fun fetchPage(isFirst: Boolean) {
        isLoading = true
        callSession.getContacts(nextCursor, PAGE_SIZE, object :
            TXIoTValueCallback<TXIoTCallSession.ContactsResult> {
            override fun onSuccess(value: TXIoTCallSession.ContactsResult?) {
                runOnUiThread {
                    isLoading = false
                    loadingContainer.visibility = View.GONE
                    val list = value?.contacts.orEmpty()
                    list.forEach {
                        val alreadyLocal = LocalContactStore.contains(
                            this@ContactsActivity, it.userId.orEmpty()
                        )
                        if (alreadyLocal) return@forEach
                        contacts.add(
                            ContactEntry(
                                it,
                                LocalContactStore.TYPE_VOIP,
                                isLocal = false
                            )
                        )
                    }
                    nextCursor = value?.nextCursor.orEmpty()
                    hasMore = nextCursor.isNotEmpty() && list.isNotEmpty()

                    footerLoading = false
                    showFooter = !hasMore && contacts.isNotEmpty()

                    if (contacts.isEmpty()) {
                        emptyView.visibility = View.VISIBLE
                        recyclerView.visibility = View.GONE
                    } else {
                        emptyView.visibility = View.GONE
                        recyclerView.visibility = View.VISIBLE
                    }
                    adapter.notifyDataSetChanged()
                }
            }

            override fun onError(code: Int, desc: String?) {
                runOnUiThread {
                    isLoading = false
                    loadingContainer.visibility = View.GONE
                    footerLoading = false
                    showFooter = false
                    if (isFirst && contacts.isEmpty()) {
                        emptyView.visibility = View.VISIBLE
                        recyclerView.visibility = View.GONE
                    } else {
                        recyclerView.visibility = View.VISIBLE
                    }
                    Toast.makeText(
                        this@ContactsActivity,
                        getString(R.string.contacts_load_error, desc.orEmpty()),
                        Toast.LENGTH_SHORT
                    ).show()
                    adapter.notifyDataSetChanged()
                }
            }
        })
    }

    private fun showCallMediaSheet(contact: TXIoTCallSession.Contact) {
        val dialog = BottomSheetDialog(
            this,
            com.google.android.material.R.style.Theme_Design_Light_BottomSheetDialog
        )
        val view = LayoutInflater.from(this)
            .inflate(R.layout.dialog_call_media, null, false)

        view.findViewById<View>(R.id.ll_call_media_video).setOnClickListener {
            dialog.dismiss()
            startCall(contact, isVideo = true)
        }
        view.findViewById<View>(R.id.ll_call_media_audio).setOnClickListener {
            dialog.dismiss()
            startCall(contact, isVideo = false)
        }
        view.findViewById<View>(R.id.tv_call_media_cancel).setOnClickListener {
            dialog.dismiss()
        }
        dialog.setContentView(view)
        (view.parent as? View)?.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        dialog.show()
    }

    private data class ContactEntry(
        val contact: TXIoTCallSession.Contact,
        val type: Int,
        val isLocal: Boolean
    )

    private fun startCall(contact: TXIoTCallSession.Contact, isVideo: Boolean) {
        val peerId = contact.userId.orEmpty()
        if (peerId.isEmpty()) return
        val mediaFlag = if (isVideo) 2 else 0
        val intent = Intent(this, CallActivity::class.java).apply {
            putExtra(CallActivity.EXTRA_MODE, CallActivity.MODE_OUTGOING)
            putExtra(CallActivity.EXTRA_PEER_ID, peerId)
            putExtra(
                CallActivity.EXTRA_PEER_NAME,
                contact.userName?.takeIf { it.isNotEmpty() } ?: peerId
            )
            putExtra(CallActivity.EXTRA_MEDIA, mediaFlag)
            addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
        }
        startActivity(intent)
    }

    private inner class ContactAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {

        override fun getItemViewType(position: Int): Int {
            return if (showFooter && position == contacts.size) VIEW_TYPE_FOOTER
            else VIEW_TYPE_CONTACT
        }

        override fun getItemCount(): Int {
            return contacts.size + if (showFooter) 1 else 0
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
            val inflater = LayoutInflater.from(parent.context)
            return if (viewType == VIEW_TYPE_FOOTER) {
                FooterHolder(inflater.inflate(R.layout.item_contact_footer, parent, false))
            } else {
                val holder = ContactHolder(inflater.inflate(R.layout.item_contact, parent, false))
                holder.swipe.listener = object : SwipeRevealLayout.OnSwipeStateChangedListener {
                    override fun onOpened(view: SwipeRevealLayout) {
                        if (openedSwipe != null && openedSwipe !== view) {
                            openedSwipe?.close()
                        }
                        openedSwipe = view
                    }
                }
                holder.content.setOnClickListener {
                    if (openedSwipe != null) {
                        openedSwipe?.close()
                        openedSwipe = null
                        return@setOnClickListener
                    }
                    if (holder.swipe.isOpen) {
                        holder.swipe.close()
                        return@setOnClickListener
                    }
                    val pos = holder.adapterPosition
                    if (pos == RecyclerView.NO_POSITION) return@setOnClickListener
                    val entry = contacts.getOrNull(pos) ?: return@setOnClickListener
                    showCallMediaSheet(entry.contact)
                }
                holder.delete.setOnClickListener {
                    val pos = holder.adapterPosition
                    if (pos == RecyclerView.NO_POSITION) return@setOnClickListener
                    val entry = contacts.getOrNull(pos) ?: return@setOnClickListener
                    onDeleteLocalContact(pos, entry)
                }
                holder
            }
        }

        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            when (holder) {
                is ContactHolder -> {
                    val entry = contacts[position]
                    val item = entry.contact
                    val displayName = item.userName?.takeIf { it.isNotEmpty() }
                        ?: "null"
                    holder.name.text = displayName
                    holder.id.text = item.userId.orEmpty()
                    bindTypeTag(holder.tag, entry.type)
                    holder.swipe.close(animate = false)
                    holder.swipe.isSwipeEnabled = entry.isLocal
                }
                is FooterHolder -> {
                    if (footerLoading) {
                        holder.progress.visibility = View.VISIBLE
                        holder.text.text = getString(R.string.contacts_loading)
                    } else {
                        holder.progress.visibility = View.GONE
                        holder.text.text = getString(R.string.contacts_no_more)
                    }
                }
            }
        }
    }

    private fun bindTypeTag(tag: TextView, type: Int) {
        when (type) {
            LocalContactStore.TYPE_VOIP -> {
                tag.visibility = View.VISIBLE
                tag.setText(R.string.contacts_tag_voip)
                tag.setBackgroundResource(R.drawable.bg_contact_tag_voip)
                tag.setTextColor(getColor(R.color.contact_tag_voip_text))
            }
            LocalContactStore.TYPE_IOT -> {
                tag.visibility = View.VISIBLE
                tag.setText(R.string.contacts_tag_iot)
                tag.setBackgroundResource(R.drawable.bg_contact_tag_iot)
                tag.setTextColor(getColor(R.color.contact_tag_iot_text))
            }
            else -> {
                tag.visibility = View.GONE
            }
        }
    }

    companion object {
        private const val PAGE_SIZE = 20
        private const val VIEW_TYPE_CONTACT = 0
        private const val VIEW_TYPE_FOOTER = 1
    }

    private class ContactHolder(view: View) : RecyclerView.ViewHolder(view) {
        val swipe: SwipeRevealLayout = view as SwipeRevealLayout
        val content: View = view.findViewById(R.id.ll_contact_content)
        val delete: View = view.findViewById(R.id.tv_contact_delete)
        val name: TextView = view.findViewById(R.id.tv_contact_name)
        val id: TextView = view.findViewById(R.id.tv_contact_id)
        val tag: TextView = view.findViewById(R.id.tv_contact_tag)
    }

    private class FooterHolder(view: View) : RecyclerView.ViewHolder(view) {
        val progress: ProgressBar = view.findViewById(R.id.pb_contact_footer)
        val text: TextView = view.findViewById(R.id.tv_contact_footer)
    }
}