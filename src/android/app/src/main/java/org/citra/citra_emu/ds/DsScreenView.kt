// Copyright AzalonDS Project
// Licensed under GPLv3 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ds

import android.content.Context
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import kotlin.math.ceil
import kotlin.math.floor

/**
 * One of the two DS screens.
 *
 * The core writes frames straight into this view's surface and the compositor scales
 * them to the view. With [smooth] the buffer is 256x192 and the compositor's bilinear
 * filtering shows; without it the core upscales nearest-neighbour to a whole multiple
 * close to the view size first, so the pixels stay sharp.
 */
class DsScreenView(context: Context, private val screen: Int) : SurfaceView(context),
    SurfaceHolder.Callback {

    var smooth: Boolean = true
        set(value) {
            field = value
            updateScale()
        }

    private var surfaceScale = 0

    init {
        holder.addCallback(this)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        updateScale()
    }

    private fun updateScale() {
        val scale = if (smooth || width <= 0) {
            1
        } else {
            ceil(width.toDouble() / DsNative.SCREEN_WIDTH).toInt().coerceIn(1, MAX_SCALE)
        }
        if (scale != surfaceScale) {
            surfaceScale = scale
            DsNative.setSurfaceScale(screen, scale)
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        DsNative.setSurface(screen, holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        DsNative.setSurface(screen, holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        DsNative.setSurface(screen, null)
    }

    companion object {
        /** Matches kMaxSurfaceScale in jni/ds/ds_session.h. */
        const val MAX_SCALE = 4
    }
}

/**
 * Lays the two DS screens out according to a [DsScreenLayout], keeping each one's
 * 256:192 aspect ratio and making them as large as the window allows.
 */
class DsScreensView(context: Context) : ViewGroup(context) {
    private val topScreen = DsScreenView(context, DsNative.SCREEN_TOP)
    private val bottomScreen = DsScreenView(context, DsNative.SCREEN_BOTTOM)

    var layoutMode: DsScreenLayout = DsScreenLayout.VERTICAL
        set(value) {
            field = value
            requestLayout()
        }

    /** Shows the bottom screen where the top one would be, and vice versa. */
    var swapped: Boolean = false
        set(value) {
            field = value
            requestLayout()
        }

    /** Sizes the screens to whole multiples of 256x192 when they are at least 1x. */
    var integerScale: Boolean = false
        set(value) {
            field = value
            requestLayout()
        }

    /** Bilinear filtering; off keeps the pixels sharp (see [DsScreenView.smooth]). */
    var smooth: Boolean = true
        set(value) {
            field = value
            topScreen.smooth = value
            bottomScreen.smooth = value
        }

    /**
     * Keeps the bottom of the view free for the on-screen controls when the window
     * is taller than it is wide. In landscape there is no room to spare, so the
     * controls are drawn over the screens instead, as elsewhere on Android.
     */
    var reserveForControls: Boolean = true
        set(value) {
            field = value
            requestLayout()
        }

    init {
        // onLayout measures and places both screens itself.
        addView(topScreen, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
        addView(bottomScreen, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
    }

    /** The view currently showing the touch screen, which is always the bottom one. */
    val touchScreenView: DsScreenView get() = bottomScreen

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(
            MeasureSpec.getSize(widthMeasureSpec),
            MeasureSpec.getSize(heightMeasureSpec)
        )
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val width = r - l
        val fullHeight = b - t
        if (width <= 0 || fullHeight <= 0) return

        val portrait = fullHeight >= width
        val height = if (portrait && reserveForControls) {
            (fullHeight * (1f - CONTROLS_BAND)).toInt()
        } else {
            fullHeight
        }

        val sw = DsNative.SCREEN_WIDTH
        val sh = DsNative.SCREEN_HEIGHT

        // `first` is drawn on top (vertical) or on the left (horizontal).
        val first = if (swapped) bottomScreen else topScreen
        val second = if (swapped) topScreen else bottomScreen

        when (layoutMode) {
            DsScreenLayout.VERTICAL -> {
                val scale = fit(minOf(width.toFloat() / sw, height.toFloat() / (sh * 2)))
                val w = (sw * scale).toInt()
                val h = (sh * scale).toInt()
                val x = (width - w) / 2
                val y = (height - h * 2) / 2
                place(first, x, y, w, h)
                place(second, x, y + h, w, h)
            }

            DsScreenLayout.HORIZONTAL -> {
                val scale = fit(minOf(width.toFloat() / (sw * 2), height.toFloat() / sh))
                val w = (sw * scale).toInt()
                val h = (sh * scale).toInt()
                val x = (width - w * 2) / 2
                val y = (height - h) / 2
                place(first, x, y, w, h)
                place(second, x + w, y, w, h)
            }

            DsScreenLayout.TOP_ONLY, DsScreenLayout.BOTTOM_ONLY -> {
                val shown =
                    if (layoutMode == DsScreenLayout.TOP_ONLY) topScreen else bottomScreen
                val hidden = if (shown === topScreen) bottomScreen else topScreen
                val scale = fit(minOf(width.toFloat() / sw, height.toFloat() / sh))
                val w = (sw * scale).toInt()
                val h = (sh * scale).toInt()
                place(shown, (width - w) / 2, (height - h) / 2, w, h)
                // Keep the other screen attached (and its surface alive) but off-screen,
                // so switching layouts does not have to recreate it.
                place(hidden, -sw, -sh, sw, sh)
            }
        }
    }

    companion object {
        /** Share of a portrait window's height the controls get to themselves. */
        const val CONTROLS_BAND = 0.32f
    }

    /** Rounds a screen scale down to a whole number when integer scaling is on. */
    private fun fit(scale: Float): Float =
        if (integerScale && scale >= 1f) floor(scale) else scale

    private fun place(view: DsScreenView, x: Int, y: Int, w: Int, h: Int) {
        view.measure(
            MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY)
        )
        view.layout(x, y, x + w, y + h)
    }
}
