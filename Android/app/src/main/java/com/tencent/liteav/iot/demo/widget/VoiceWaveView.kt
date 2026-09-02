package com.tencent.liteav.iot.demo.widget

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import android.view.animation.LinearInterpolator
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin

class VoiceWaveView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyle: Int = 0
) : View(context, attrs, defStyle) {

    private var barColor: Int = Color.parseColor("#22C55E")

    private var barCount: Int = 32

    private var barWidth: Float = dp(3.5f)

    private var barGap: Float = dp(4f)

    private val minRatio: Float = 0.18f

    private val maxRatio: Float = 0.85f

    private val periodMs: Long = 1200L

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = barColor
    }

    private val rect = RectF()

    private var animator: ValueAnimator? = null

    private var phase: Float = 0f

    fun setBarColor(color: Int) {
        barColor = color
        paint.color = color
        invalidate()
    }

    override fun onDetachedFromWindow() {
        stopAnim()
        super.onDetachedFromWindow()
    }

    fun startAnim() {
        if (animator?.isRunning == true) return
        animator = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = periodMs
            repeatCount = ValueAnimator.INFINITE
            interpolator = LinearInterpolator()
            addUpdateListener {
                phase = it.animatedValue as Float
                invalidate()
            }
            start()
        }
    }

    fun stopAnim() {
        animator?.cancel()
        animator = null
        phase = 0f
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0 || h <= 0) return

        val totalBarsWidth = barCount * barWidth + (barCount - 1) * barGap
        var x = (w - totalBarsWidth) / 2f
        val cy = h / 2f
        val corner = barWidth / 2f

        for (i in 0 until barCount) {
            val t = phase + i / barCount.toFloat()
            val n = (sin(t * 2 * PI).toFloat() + 1f) / 2f
            val centerBoost = 1f - abs(i - (barCount - 1) / 2f) / barCount.toFloat()
            val ratio = minRatio + (maxRatio - minRatio) * n * (0.6f + 0.4f * centerBoost)
            val barH = h * ratio
            rect.set(x, cy - barH / 2f, x + barWidth, cy + barH / 2f)
            canvas.drawRoundRect(rect, corner, corner, paint)
            x += barWidth + barGap
        }
    }

    private fun dp(value: Float): Float =
        value * resources.displayMetrics.density
}
