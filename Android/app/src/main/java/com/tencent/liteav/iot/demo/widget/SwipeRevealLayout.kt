package com.tencent.liteav.iot.demo.widget

import android.animation.ValueAnimator
import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.VelocityTracker
import android.view.View
import android.view.ViewConfiguration
import android.widget.FrameLayout
import kotlin.math.abs

/**
 * iOS 风格的左滑显露菜单容器：
 * - 子 View 数量必须为 2：第 0 个为主内容（match_parent），第 1 个为右侧菜单（wrap_content）
 * - 左滑时将主内容向左平移，右侧菜单从右边缘露出；右滑或点击外部区域则归位
 * - 可通过 [isSwipeEnabled] 关闭滑动能力（远程联系人不允许左滑）
 */
class SwipeRevealLayout @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : FrameLayout(context, attrs, defStyleAttr) {

    private var mainView: View? = null
    private var menuView: View? = null

    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private val minFlingVelocity = ViewConfiguration.get(context).scaledMinimumFlingVelocity

    private var downX = 0f
    private var downY = 0f
    private var lastX = 0f
    private var dragging = false
    private var currentOffset = 0f // 主内容当前的 translationX（负值，最小 = -menuWidth）
    private var menuWidth = 0
    private var velocityTracker: VelocityTracker? = null
    private var animator: ValueAnimator? = null

    var isSwipeEnabled: Boolean = true
        set(value) {
            if (field != value) {
                field = value
                if (!value) close(animate = false)
            }
        }

    var listener: OnSwipeStateChangedListener? = null

    interface OnSwipeStateChangedListener {
        fun onOpened(view: SwipeRevealLayout)
    }

    override fun onFinishInflate() {
        super.onFinishInflate()
        require(childCount == 2) { "SwipeRevealLayout 需要恰好 2 个子 View" }
        mainView = getChildAt(0)
        menuView = getChildAt(1)
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        super.onLayout(changed, left, top, right, bottom)
        val menu = menuView ?: return
        menuWidth = menu.measuredWidth
        // 菜单放到右侧外边缘外面，等待左滑露出
        menu.layout(
            measuredWidth,
            menu.top,
            measuredWidth + menuWidth,
            menu.top + menu.measuredHeight
        )
        applyOffset(currentOffset)
    }

    fun close(animate: Boolean = true) {
        animateTo(0f, animate)
    }

    fun open(animate: Boolean = true) {
        if (menuWidth <= 0) menuWidth = menuView?.measuredWidth ?: 0
        animateTo(-menuWidth.toFloat(), animate)
    }

    val isOpen: Boolean
        get() = currentOffset <= -menuWidth / 2f && menuWidth > 0

    private fun applyOffset(offset: Float) {
        currentOffset = offset.coerceIn(-menuWidth.toFloat(), 0f)
        mainView?.translationX = currentOffset
        menuView?.translationX = currentOffset
    }

    private fun animateTo(target: Float, animate: Boolean) {
        animator?.cancel()
        if (!animate) {
            applyOffset(target)
            if (target <= -menuWidth.toFloat() && menuWidth > 0) {
                listener?.onOpened(this)
            }
            return
        }
        val start = currentOffset
        animator = ValueAnimator.ofFloat(start, target).apply {
            duration = 180L
            addUpdateListener { applyOffset(it.animatedValue as Float) }
        }
        animator?.start()
        if (target <= -menuWidth.toFloat() && menuWidth > 0) {
            listener?.onOpened(this)
        }
    }

    override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
        if (!isSwipeEnabled) return super.onInterceptTouchEvent(ev)
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = ev.x
                downY = ev.y
                lastX = ev.x
                dragging = false
                velocityTracker?.recycle()
                velocityTracker = VelocityTracker.obtain().also { it.addMovement(ev) }
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = ev.x - downX
                val dy = ev.y - downY
                if (!dragging && abs(dx) > touchSlop && abs(dx) > abs(dy)) {
                    // 只有横向滑动才拦截
                    dragging = true
                    parent?.requestDisallowInterceptTouchEvent(true)
                    return true
                }
                // 已展开时无论手指向哪个方向拖动，都优先由自己处理，防止点击穿透
                if (currentOffset < 0f && abs(dx) > touchSlop) {
                    dragging = true
                    parent?.requestDisallowInterceptTouchEvent(true)
                    return true
                }
            }
        }
        return super.onInterceptTouchEvent(ev)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!isSwipeEnabled) return super.onTouchEvent(event)
        velocityTracker?.addMovement(event)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                lastX = event.x
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.x - lastX
                lastX = event.x
                if (!dragging) {
                    if (abs(event.x - downX) > touchSlop) {
                        dragging = true
                        parent?.requestDisallowInterceptTouchEvent(true)
                    }
                }
                if (dragging) {
                    applyOffset(currentOffset + dx)
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val wasDragging = dragging
                dragging = false
                velocityTracker?.computeCurrentVelocity(1000)
                val vx = velocityTracker?.xVelocity ?: 0f
                velocityTracker?.recycle()
                velocityTracker = null
                parent?.requestDisallowInterceptTouchEvent(false)

                if (wasDragging) {
                    val threshold = -menuWidth / 2f
                    val shouldOpen = when {
                        vx <= -minFlingVelocity -> true
                        vx >= minFlingVelocity -> false
                        else -> currentOffset <= threshold
                    }
                    if (shouldOpen) open() else close()
                    return true
                }
            }
        }
        return super.onTouchEvent(event)
    }
}
