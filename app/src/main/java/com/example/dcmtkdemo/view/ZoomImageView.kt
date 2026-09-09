package com.example.dcmtkdemo.view

import android.content.Context
import android.graphics.Matrix
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import androidx.appcompat.widget.AppCompatImageView

/**
 * 自定义手势缩放 ImageView (0.5x - 5.0x)
 * 优化：解决内容更新重置、适配 FitCenter 逻辑及滑动冲突
 */
class ZoomImageView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : AppCompatImageView(context, attrs, defStyleAttr), View.OnTouchListener {

    private val mMatrix = Matrix()
    private val mMatrixValues = FloatArray(9)

    private var mScaleGestureDetector: ScaleGestureDetector? = null
    private var mGestureDetector: GestureDetector? = null

    private var mMinScale = 0.1f 
    private var mMaxScale = 5.0f
    private var mBaseScale = 1.0f

    init {
        scaleType = ScaleType.MATRIX
        setOnTouchListener(this)

        mScaleGestureDetector = ScaleGestureDetector(context, object : ScaleGestureDetector.OnScaleGestureListener {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                val scale = getScale()
                var scaleFactor = detector.scaleFactor
                if (drawable == null) return true

                if ((scale < mMaxScale && scaleFactor > 1.0f) || (scale > mMinScale && scaleFactor < 1.0f)) {
                    if (scaleFactor * scale < mMinScale) scaleFactor = mMinScale / scale
                    if (scaleFactor * scale > mMaxScale) scaleFactor = mMaxScale / scale
                    
                    mMatrix.postScale(scaleFactor, scaleFactor, detector.focusX, detector.focusY)
                    checkBorderAndCenterWhenScale()
                    imageMatrix = mMatrix
                }
                return true
            }
            override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
                parent?.requestDisallowInterceptTouchEvent(true)
                return true
            }
            override fun onScaleEnd(detector: ScaleGestureDetector) {}
        })

        mGestureDetector = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
            override fun onScroll(e1: MotionEvent?, e2: MotionEvent, distanceX: Float, distanceY: Float): Boolean {
                if (drawable == null) return false
                mMatrix.postTranslate(-distanceX, -distanceY)
                checkBorderAndCenterWhenScale()
                imageMatrix = mMatrix
                return true
            }

            override fun onDoubleTap(e: MotionEvent): Boolean {
                if (drawable == null) return false
                val scale = getScale()
                if (scale < mBaseScale * 1.5f) {
                    val targetScale = Math.min(mBaseScale * 2.5f, mMaxScale)
                    val factor = targetScale / scale
                    mMatrix.postScale(factor, factor, e.x, e.y)
                } else {
                    resetMatrix()
                }
                checkBorderAndCenterWhenScale()
                imageMatrix = mMatrix
                return true
            }
        })
    }

    override fun setImageDrawable(drawable: Drawable?) {
        super.setImageDrawable(drawable)
        // 关键：当预处理或调窗产生新 Bitmap 时，立即重置 Matrix
        resetMatrix()
    }

    private fun getScale(): Float {
        mMatrix.getValues(mMatrixValues)
        return mMatrixValues[Matrix.MSCALE_X]
    }

    override fun onTouch(v: View, event: MotionEvent): Boolean {
        // 让两个探测器都处理事件
        mScaleGestureDetector?.onTouchEvent(event)
        mGestureDetector?.onTouchEvent(event)

        when (event.action and MotionEvent.ACTION_MASK) {
            MotionEvent.ACTION_DOWN -> {
                val rect = getMatrixRectF()
                // 如果图片已放大，按下时就禁止父容器拦截，确保能立即拖动
                if (rect.width() > width + 1f || rect.height() > height + 1f) {
                    parent?.requestDisallowInterceptTouchEvent(true)
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                // 多指按下，绝对禁止父容器拦截（准备缩放）
                parent?.requestDisallowInterceptTouchEvent(true)
            }
            MotionEvent.ACTION_MOVE -> {
                val rect = getMatrixRectF()
                // 移动过程中根据缩放状态动态判断是否拦截
                if (rect.width() > width + 1f || rect.height() > height + 1f || event.pointerCount >= 2) {
                    parent?.requestDisallowInterceptTouchEvent(true)
                } else {
                    parent?.requestDisallowInterceptTouchEvent(false)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                parent?.requestDisallowInterceptTouchEvent(false)
            }
        }
        return true
    }

    private fun getMatrixRectF(): RectF {
        val rect = RectF()
        drawable?.let {
            rect.set(0f, 0f, it.intrinsicWidth.toFloat(), it.intrinsicHeight.toFloat())
            mMatrix.mapRect(rect)
        }
        return rect
    }

    private fun checkBorderAndCenterWhenScale() {
        val rect = getMatrixRectF()
        var deltaX = 0f
        var deltaY = 0f
        val viewWidth = width
        val viewHeight = height

        if (rect.height() >= viewHeight) {
            if (rect.top > 0) deltaY = -rect.top
            if (rect.bottom < viewHeight) deltaY = viewHeight - rect.bottom
        } else {
            deltaY = viewHeight / 2f - rect.bottom + rect.height() / 2f
        }

        if (rect.width() >= viewWidth) {
            if (rect.left > 0) deltaX = -rect.left
            if (rect.right < viewWidth) deltaX = viewWidth - rect.right
        } else {
            deltaX = viewWidth / 2f - rect.right + rect.width() / 2f
        }
        mMatrix.postTranslate(deltaX, deltaY)
    }

    /**
     * 重置 Matrix：默认完整显示图像 (FitCenter)
     */
    private fun resetMatrix() {
        val d = drawable ?: return
        val viewWidth = width
        val viewHeight = height
        if (viewWidth <= 0 || viewHeight <= 0) return

        val dw = d.intrinsicWidth
        val dh = d.intrinsicHeight

        mMatrix.reset()
        val srcRect = RectF(0f, 0f, dw.toFloat(), dh.toFloat())
        val dstRect = RectF(0f, 0f, viewWidth.toFloat(), viewHeight.toFloat())
        
        // 使用官方标准算法将图片缩放到 FitCenter 状态
        mMatrix.setRectToRect(srcRect, dstRect, Matrix.ScaleToFit.CENTER)
        
        // 基于 FitCenter 后的比例更新最小/最大缩放限制
        mBaseScale = getScale()
        mMinScale = mBaseScale * 0.8f
        mMaxScale = Math.max(mBaseScale * 10f, 5.0f)
        
        imageMatrix = mMatrix
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        super.onLayout(changed, left, top, right, bottom)
        // 布局尺寸变化时重置（如横竖屏切换）
        resetMatrix()
    }
}
