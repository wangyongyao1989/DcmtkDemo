package com.wangyao.cbctmeasure.report

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.pdf.PdfDocument

/**
 * PDF 分页器（ReportGenerator 的排版底座）。
 *
 * PdfDocument 只提供"页画布"，没有 flowing layout，所以行高、表格换列、
 * 剩余空间判断全部在这里手工实现。A4 在 72dpi 下是 595×842pt，
 * 因此这里所有尺寸都用 pt（与页面坐标 1:1），不乘 density ——
 * 手机上 density 会把版式放大到溢出，这是这类 API 最常见的坑。
 *
 * 页脚只写"第 N 页"：总页数要等 finish() 才知道，回写已 finishPage 的页面
 * PdfDocument 不支持，所以不做"共 M 页"这种伪精确的表述。
 *
 * 【铁律：canvas 绝对不能跨页缓存】
 * 这里踩过一个真机 SIGSEGV：`val cc = c()` 之后再调 ensure()/newPage()，
 * 一旦换页，旧页已经 doc.finishPage(page) 交给文档，其 native SkCanvas 被销毁，
 * 但 Java 侧 Canvas 仍持有那个野指针 —— 下一次 cc.drawText() 直接读到已释放对象，
 * 崩在 libhwui 的 android::Canvas::drawText+300（fault addr 0x8，null pointer dereference），
 * 而且是在协程线程上崩的，Kotlin 的 try/catch 拦不住 native crash。
 * 内容少的时候永远不换页，所以小数据量导出一切正常；条目一多、某段文本正好压到页底，
 * 就必崩。因此本类约定：任何可能触发换页的调用（ensure/newPage）之后，一律重新取 canvas，
 * 并且统一走 surface(need) —— 它先保证空间、再返回"这一笔真正该落在的那一页"的 canvas。
 */
internal class Pager(private val doc: PdfDocument) {

    private val pageW = 595f
    private val pageH = 842f
    private val marginL = 40f
    private val marginR = 40f
    private val marginTop = 40f
    private val marginBottom = 56f

    private val footPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 8f
        color = Color.DKGRAY
    }
    private val rulePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        strokeWidth = 0.6f
        color = Color.rgb(190, 190, 190)
    }
    private val boxPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 0.6f
        color = Color.rgb(150, 150, 150)
    }
    private val headBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.rgb(232, 240, 250)
    }

    private var canvas: Canvas? = null
    private var currentPage: PdfDocument.Page? = null
    private var pageNo = 0
    private var y = marginTop
    private val colX = ArrayList<Float>(8)
    private var rowHeight = 14f

    private val innerW = pageW - marginL - marginR

    fun contentWidth(): Float = innerW

    fun startPage() {
        newPage()
    }

    /** 结束当前页并开启新页；调用方负责之后的内容 */
    fun newPage() {
        closeCurrentPage()
        pageNo += 1
        val info = PdfDocument.PageInfo.Builder(pageW.toInt(), pageH.toInt(), pageNo).create()
        val page = doc.startPage(info)
        currentPage = page
        canvas = page.canvas
        y = marginTop
    }

    /** 收尾：返回总页数 */
    fun finish(): Int {
        closeCurrentPage()
        canvas = null
        currentPage = null
        return pageNo
    }

    /**
     * 页脚必须在 finishPage 之前写：PdfDocument 的页一旦交出就不可再改，
     * 这也是这里不写"共 M 页"的原因（总页数此刻还不知道）。
     */
    private fun closeCurrentPage() {
        val page = currentPage ?: return
        stampFooter()
        runCatching { doc.finishPage(page) }
    }

    /** 异常路径：不再向文档追加任何页，避免 write 时留下半页 */
    fun abort() {
        canvas = null
        currentPage = null
    }

    fun cursorX(): Float = marginL
    fun cursorY(): Float = y
    fun hasHeight(need: Float): Boolean = y + need <= pageH - marginBottom
    fun remainingHeight(): Float = (pageH - marginBottom - y).coerceAtLeast(0f)

    private fun c(): Canvas? = canvas

    private fun ensure(need: Float) {
        if (!hasHeight(need)) newPage()
    }

    /**
     * "保证这一行放得下，并把画笔交给我" —— 本类唯一的取 canvas 入口。
     *
     * 必须在返回之后立刻落笔、不得把返回值存到下一次 ensure 之前再用：
     * 换页会销毁上一页的 native canvas，跨页使用即野指针（见类注释的崩溃现场）。
     */
    private fun surface(need: Float): Canvas? {
        ensure(need)
        return c()
    }

    fun addLine(h: Float) {
        y += h
    }

    /** 当前页可写的最大文本宽度（供调用方测量列宽） */
    fun textWidth(paint: Paint, text: String): Float = paint.measureText(text)

    fun horizontalRule(paint: Paint) {
        val cc = c() ?: return
        cc.drawLine(marginL, y, pageW - marginR, y, paint)
    }

    fun drawText(paint: Paint, text: String) {
        if (text.isEmpty()) return
        val lineH = (paint.textSize * 1.35f)
        for (line in wrap(text, paint, innerW)) {
            // 每行单独取 canvas：上一行可能刚把页面写满并触发换页
            val cc = surface(lineH) ?: return
            cc.drawText(line, marginL, y + paint.textSize, paint)
            y += lineH
        }
    }

    fun section(paint: Paint, title: String) {
        val lineH = paint.textSize * 1.6f
        val cc = surface(lineH + 10f) ?: return
        y += 4f
        cc.drawText(title, marginL, y + paint.textSize, paint)
        y += lineH
        cc.drawLine(marginL, y - 2f, pageW - marginR, y - 2f, rulePaint)
    }

    /** 键值两栏（左标签占 26% 宽度，右侧自动换行） */
    fun keyValue(valuePaint: Paint, labelPaint: Paint, key: String, value: String) {
        val labelW = innerW * 0.26f
        val lineH = valuePaint.textSize * 1.4f
        val maxW = innerW - labelW
        var firstRow = true
        for (line in wrap(value, valuePaint, maxW)) {
            val cc = surface(lineH) ?: return
            // 基线一律用换页之后的 y 现算：surface 可能刚把游标搬到新页顶部，
            // 沿用换页前算好的 ty 会把文字画到页眉上方（也踩过的坑）
            val baseline = y + valuePaint.textSize
            if (firstRow) {
                cc.drawText(key, marginL, baseline, labelPaint)
                firstRow = false
            }
            cc.drawText(line, marginL + labelW, baseline, valuePaint)
            y += lineH
        }
    }

    /**
     * 表头：weights 为各列宽度占比（会自动归一化）。
     * 表头行带浅底色，与项目 UI 的表格观感保持一致。
     */
    fun tableHeader(paint: Paint, titles: Array<String>, weights: FloatArray) {
        setupColumns(weights)
        val h = paint.textSize * 1.6f
        val cc = surface(h) ?: return
        cc.drawRoundRect(RectF(marginL, y, pageW - marginR, y + h), 2f, 2f, headBgPaint)
        for (i in titles.indices) {
            cc.drawText(titles[i], colX[i], y + paint.textSize * 1.15f, paint)
        }
        y += h
        rowHeight = h
    }

    /** 表格数据行：任一单元格超列宽就换行，整行高度取最高的一格 */
    fun tableRow(paint: Paint, cells: Array<String>) {
        val lineH = paint.textSize * 1.35f
        val lines = ArrayList<List<String>>(cells.size)
        var maxLines = 1
        for (i in cells.indices) {
            val w = if (i + 1 < colX.size) colX[i + 1] - colX[i] - 4f else innerW * 0.2f
            val wrapped = wrap(cells[i], paint, w.coerceAtLeast(20f))
            lines.add(wrapped)
            if (wrapped.size > maxLines) maxLines = wrapped.size
        }
        val rowH = (lineH * maxLines).coerceAtLeast(13f)
        val cc = surface(rowH) ?: return
        val top = y
        for (i in cells.indices) {
            var ty = top + paint.textSize * 1.1f
            for (line in lines[i]) {
                cc.drawText(line, colX[i], ty, paint)
                ty += lineH
            }
        }
        cc.drawLine(marginL, top + rowH, pageW - marginR, top + rowH, rulePaint)
        y = top + rowH
    }

    /** 证据图：由调用方算好目标矩形，这里只负责确保放得下 */
    fun drawBitmap(bmp: Bitmap, dst: RectF, border: Paint) {
        val cc = c() ?: return
        cc.drawBitmap(bmp, null, dst, border)
        cc.drawRect(dst, boxPaint)
    }

    fun footer(text: String) {
        val cc = c() ?: return
        val fy = pageH - marginBottom + 22f
        cc.drawText("第 $pageNo 页", pageW - marginR - footPaint.measureText("第 $pageNo 页"), fy, footPaint)
        cc.drawText(text, marginL, fy, footPaint)
    }

    /** 每页收尾时补页脚：finish/newPage 前都会调用 */
    private fun stampFooter() {
        footer("CBCT 测量报告 · 软件自动测量，须临床复核")
    }

    private fun setupColumns(weights: FloatArray) {
        colX.clear()
        var sum = 0f
        for (w in weights) sum += w
        val norm = if (sum <= 0f) weights.map { 1f / weights.size } else weights.map { it / sum }
        var x = marginL
        for (w in norm) {
            colX.add(x + 2f)
            x += innerW * w
        }
    }

    /**
     * 按像素宽度断行。
     * 中英文混排：优先在空格处断，没有空格就按字符累积宽度断（中文没有空格）。
     */
    private fun wrap(text: String, paint: Paint, maxWidth: Float): List<String> {
        if (text.isEmpty() || maxWidth <= 0f) return listOf("")
        val out = ArrayList<String>(4)
        val sb = StringBuilder()
        var lineWidth = 0f
        for (ch in text) {
            val cw = paint.measureText(ch.toString())
            if (lineWidth + cw > maxWidth && sb.isNotEmpty()) {
                out.add(sb.toString())
                sb.setLength(0)
                lineWidth = 0f
            }
            sb.append(ch)
            lineWidth += cw
        }
        if (sb.isNotEmpty()) out.add(sb.toString())
        return if (out.isEmpty()) listOf(text) else out
    }
}
