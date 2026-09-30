package com.wangyao.cbctmeasure.report

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.pdf.PdfDocument
import android.util.Log
import com.wangyao.cbctmeasure.jni.AnnotationJni
import com.wangyao.cbctmeasure.jni.MeasureJni
import com.wangyao.cbctmeasure.jni.RoiJni
import com.wangyao.cbctmeasure.jni.SurgeryPlanJni
import com.wangyao.cbctmeasure.model.AnnotationItem
import com.wangyao.cbctmeasure.model.AnnotationType
import com.wangyao.cbctmeasure.model.MeasureItem
import com.wangyao.cbctmeasure.model.MeasureType
import com.wangyao.cbctmeasure.model.RoiItem
import com.wangyao.cbctmeasure.model.RoiType
import com.wangyao.cbctmeasure.model.SafetyLevel
import com.wangyao.cbctmeasure.model.VolumeInfo
import com.wangyao.cbctmeasure.store.StorePaths
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * PDF 测量报告生成（PRD 5.5.2 的报告内容结构，验收项 AC-08）。
 *
 * 为什么用 android.graphics.pdf.PdfDocument 而不是 iText/PdfBox：
 * PRD 4.1 要求 Phase 1 不引入第三方依赖；PdfDocument 是 framework 自带（API 21+），
 * 输出的是真 PDF 矢量页，文字可检索、可打印，代价是没有富文本排版，
 * 因此这里自己实现一个分页器（Pager）来管行高与页眉页脚。
 *
 * 数值一律来自 MeasureJni.records() 等接口，本类只做排版，不做任何计算或单位换算。
 */
class ReportGenerator(private val context: Context) {

    /** 生成结果：file 为 PDF 路径，pages/counts 供 UI 与日志核对 */
    data class Output(
        val ok: Boolean,
        val file: java.io.File,
        val pages: Int,
        val message: String,
    )

    private val titlePaint = paint(18f, Color.BLACK)
    private val headPaint = paint(13f, Color.BLACK, bold = true)
    private val colPaint = paint(9.5f, Color.BLACK, bold = true)
    private val bodyPaint = paint(9.5f, Color.BLACK)
    private val dimPaint = paint(9f, Color.DKGRAY)
    private val footPaint = paint(8f, Color.DKGRAY)
    private val linePaint = paint(0.8f, Color.rgb(170, 170, 170))
    private val rulePaint = paint(1.2f, Color.rgb(60, 110, 180))

    /**
     * 生成报告。
     * @param handle 会话句柄（数据唯一来源）
     * @param info 体数据概况（患者/序列信息，来自 MeasureJni.dumpVolumeInfo）
     * @param evidence 证据图（CbctMeasureView.captureEvidence()，含叠加图形）
     * @param operatorName 操作者（写入页眉，SR 用同一值）
     */
    fun generate(
        handle: Long,
        info: VolumeInfo,
        evidence: Bitmap?,
        operatorName: String,
    ): Output {
        if (handle == 0L) return Output(false, context.filesDir, 0, "会话未创建")
        val records = MeasureJni.records(handle)
        val rois = RoiJni.list(handle)
        val planJson = MeasureJni.dumpPlan(handle)
        val implants = SurgeryPlanJni.implantsOf(planJson)
        val nerves = SurgeryPlanJni.nervesOf(planJson)
        val annotations = AnnotationJni.list(handle)
        val stamp = System.currentTimeMillis()
        val file = StorePaths.reportFile(context, info.studyInstanceUID, stamp)

        val doc = PdfDocument()
        val pager = Pager(doc)
        return try {
            pager.startPage()
            drawTitle(pager, info, operatorName)
            drawPatientBlock(pager, info, records.size + rois.size + implants.size + annotations.size)
            drawMeasureTable(pager, records)
            drawRoiTable(pager, rois)
            drawImplantTable(pager, implants, nerves)
            drawAnnotationTable(pager, annotations)
            drawEvidence(pager, evidence)
            drawDisclaimer(pager)
            val pages = pager.finish()
            file.parentFile?.mkdirs()
            FileOutputStream(file).use { doc.writeTo(it) }
            Log.i(
                TAG, "report ok pages=$pages measures=${records.size} rois=${rois.size}" +
                        " implants=${implants.size} nerves=${nerves.size} annos=${annotations.size}" +
                        " evidence=${evidence != null} -> ${file.absolutePath}"
            )
            Output(file.exists(), file, pages, if (file.exists()) "报告已生成（$pages 页）" else "PDF 写入失败")
        } catch (e: Exception) {
            Log.e(TAG, "generate report failed", e)
            pager.abort()
            Output(false, file, 0, "生成异常：${e.message}")
        } finally {
            runCatching { doc.close() }
        }
    }

    // =========================================================================
    // 各章节
    // =========================================================================

    private fun drawTitle(pager: Pager, info: VolumeInfo, operator: String) {
        pager.drawText(titlePaint, "CBCT 测量与手术规划报告")
        pager.addLine(6f)
        pager.drawText(
            dimPaint,
            "生成时间 ${TIME_FMT.format(Date())}    操作者 ${operator.ifEmpty { "-" }}"
        )
        pager.drawText(
            dimPaint,
            "序列 ${info.seriesDescription.ifEmpty { "-" }}    Study ${shortUid(info.studyInstanceUID)}"
        )
        pager.addLine(4f)
        pager.horizontalRule(rulePaint)
        pager.addLine(8f)
    }

    private fun drawPatientBlock(pager: Pager, info: VolumeInfo, itemCount: Int) {
        pager.section(headPaint, "一、患者与数据")
        val rows = listOf(
            "患者姓名" to info.patientName.ifEmpty { "-" },
            "患者编号" to info.patientID.ifEmpty { "-" },
            "性别 / 出生日期" to "${info.patientSex.ifEmpty { "-" }} / ${info.studyDateEmptyAware()}",
            "检查日期" to info.studyDate.ifEmpty { "-" },
            "设备厂商" to info.manufacturer.ifEmpty { "-" },
            "矩阵 / 体素" to "${info.width} × ${info.height} × ${info.depth}，${info.voxelSizeText()}",
            "Series UID" to shortUid(info.seriesInstanceUID),
            "数据最大对角径" to String.format(Locale.US, "%.2f mm", info.maxDiagonalMm),
            "本次报告条目数" to "$itemCount",
        )
        for ((k, v) in rows) pager.keyValue(bodyPaint, dimPaint, k, v)
        pager.addLine(8f)
    }

    private fun drawMeasureTable(pager: Pager, records: List<MeasureItem>) {
        pager.section(headPaint, "二、测量结果（M-01 ~ M-08）")
        if (records.isEmpty()) {
            pager.drawText(dimPaint, "（本次无测量项）")
            pager.addLine(6f)
            return
        }
        pager.tableHeader(
            colPaint,
            arrayOf("序号", "类型", "名称", "数值", "ROI", "备注"),
            floatArrayOf(0.09f, 0.14f, 0.24f, 0.19f, 0.08f, 0.26f),
        )
        for (r in records) {
            val value = if (r.isFailed()) "待重算" else r.displayValue()
            pager.tableRow(
                bodyPaint,
                arrayOf(
                    r.id.toString(),
                    MeasureType.label(r.type),
                    r.name.ifEmpty { "-" },
                    value,
                    if (r.roiId > 0) r.roiId.toString() else "-",
                    r.note.ifEmpty { r.errorText() }.ifEmpty { "-" },
                )
            )
        }
        pager.addLine(8f)
    }

    private fun drawRoiTable(pager: Pager, rois: List<RoiItem>) {
        pager.section(headPaint, "三、兴趣区（R-01 ~ R-05）")
        if (rois.isEmpty()) {
            pager.drawText(dimPaint, "（本次无 ROI）")
            pager.addLine(6f)
            return
        }
        pager.tableHeader(
            colPaint,
            arrayOf("序号", "类型", "名称", "参数", "点数"),
            floatArrayOf(0.09f, 0.16f, 0.25f, 0.40f, 0.10f),
        )
        for (roi in rois) {
            pager.tableRow(
                bodyPaint,
                arrayOf(
                    roi.id.toString(),
                    RoiType.label(roi.type),
                    roi.name.ifEmpty { "-" },
                    roiParameterText(roi),
                    roi.polygon.size.toString(),
                )
            )
        }
        pager.addLine(8f)
    }

    private fun drawImplantTable(
        pager: Pager,
        implants: List<com.wangyao.cbctmeasure.model.ImplantItem>,
        nerves: List<com.wangyao.cbctmeasure.model.NervePathItem>,
    ) {
        pager.section(headPaint, "四、种植体规划与安全评估（S-01 ~ S-06）")
        if (implants.isEmpty()) {
            pager.drawText(dimPaint, "（本次无种植体）")
        } else {
            pager.tableHeader(
                colPaint,
                arrayOf("序号", "名称", "规格 mm", "入口(mm)", "骨高", "骨宽", "神经", "间距", "判定"),
                floatArrayOf(0.06f, 0.15f, 0.13f, 0.20f, 0.09f, 0.09f, 0.08f, 0.08f, 0.12f),
            )
            for (im in implants) {
                pager.tableRow(
                    bodyPaint,
                    arrayOf(
                        im.id.toString(),
                        im.name.ifEmpty { "-" },
                        String.format(Locale.US, "φ%.1f×%.1f", im.diaMm, im.lengthMm),
                        String.format(Locale.US, "%.1f,%.1f,%.1f", im.entry.x, im.entry.y, im.entry.z),
                        String.format(Locale.US, "%.1f", im.boneHeightMm),
                        String.format(Locale.US, "%.1f", im.boneWidthMm),
                        String.format(Locale.US, "%.1f", im.nerveDistMm),
                        String.format(Locale.US, "%.1f", im.minSpacingMm),
                        SafetyLevel.label(im.level),
                    )
                )
                // 告警单独一行，中文较长，塞进表格列会被裁掉
                if (im.warnText.isNotEmpty()) {
                    pager.drawText(dimPaint, "    提示：${im.warnText}")
                }
            }
        }
        if (nerves.isNotEmpty()) {
            pager.addLine(4f)
            for (np in nerves) {
                pager.drawText(
                    bodyPaint,
                    "神经管 ${np.name.ifEmpty { "#${np.id}" }}：${np.points.size} 点，" +
                            "半径 ${String.format(Locale.US, "%.1f", np.radiusMm)} mm"
                )
            }
        }
        pager.addLine(8f)
    }

    private fun drawAnnotationTable(pager: Pager, annotations: List<AnnotationItem>) {
        pager.section(headPaint, "五、人工标注（A-01 ~ A-07）")
        if (annotations.isEmpty()) {
            pager.drawText(dimPaint, "（本次无标注）")
            pager.addLine(6f)
            return
        }
        pager.tableHeader(
            colPaint,
            arrayOf("序号", "类型", "内容", "关联测量"),
            floatArrayOf(0.09f, 0.18f, 0.58f, 0.15f),
        )
        for (a in annotations) {
            pager.tableRow(
                bodyPaint,
                arrayOf(
                    a.id.toString(),
                    AnnotationType.label(a.type),
                    a.text.ifEmpty { "${a.points.size} 点" },
                    if (a.measureId > 0) a.measureId.toString() else "-",
                )
            )
        }
        pager.addLine(8f)
    }

    private fun drawEvidence(pager: Pager, evidence: Bitmap?) {
        pager.section(headPaint, "六、图像证据")
        if (evidence == null || evidence.width < 2 || evidence.height < 2) {
            pager.drawText(dimPaint, "（未获取到渲染帧：请在体数据绘制完成后再导出报告）")
            pager.addLine(6f)
            return
        }
        val maxW = pager.contentWidth()
        val maxH = pager.remainingHeight() * 0.7f
        val scale = minOf(maxW / evidence.width, maxH / evidence.height)
        val w = (evidence.width * scale).toInt().coerceAtLeast(1)
        val h = (evidence.height * scale).toInt().coerceAtLeast(1)
        // 证据图需要另起一页时不裁半张：剩余高度不够就换页
        if (!pager.hasHeight(h + 26f)) {
            pager.newPage()
        }
        val dst = RectF(pager.cursorX(), pager.cursorY(), pager.cursorX() + w, pager.cursorY() + h)
        pager.drawBitmap(evidence, dst, linePaint)
        pager.addLine(h + 4f)
        pager.drawText(dimPaint, "上图为导出时刻的渲染画面，叠加图形与屏幕所见一致（同一渲染器投影）")
        pager.addLine(8f)
    }

    private fun drawDisclaimer(pager: Pager) {
        pager.horizontalRule(linePaint)
        pager.addLine(4f)
        pager.drawText(
            footPaint,
            "本报告由软件自动测量生成，所有数值依赖体数据质量与拾取位置，须经临床复核后方可用于诊断与手术决策。"
        )
    }

    // =========================================================================
    // 文本工具
    // =========================================================================

    private fun roiParameterText(roi: RoiItem): String = when (roi.type) {
        RoiType.HU_THRESHOLD -> String.format(Locale.US, "HU %.0f ~ %.0f", roi.huMin, roi.huMax)
        RoiType.BOX -> String.format(
            Locale.US, "(%.1f,%.1f,%.1f) ~ (%.1f,%.1f,%.1f)",
            roi.boxMin.x, roi.boxMin.y, roi.boxMin.z, roi.boxMax.x, roi.boxMax.y, roi.boxMax.z
        )
        RoiType.PLANE -> String.format(
            Locale.US, "原点(%.1f,%.1f,%.1f) 法线(%.1f,%.1f,%.1f)",
            roi.planeOrigin.x, roi.planeOrigin.y, roi.planeOrigin.z,
            roi.planeNormal.x, roi.planeNormal.y, roi.planeNormal.z
        )
        RoiType.SPHERE -> String.format(
            Locale.US, "球心(%.1f,%.1f,%.1f) R=%.1f",
            roi.sphereCenter.x, roi.sphereCenter.y, roi.sphereCenter.z, roi.sphereRadius
        )
        RoiType.COMPOSITE -> "子 ROI ${roi.childA}/${roi.childB}/${roi.childC}"
        else -> "-"
    }

    private fun shortUid(uid: String): String =
        if (uid.length <= 24) uid.ifEmpty { "-" } else uid.substring(uid.length - 12)

    private fun paint(size: Float, color: Int, bold: Boolean = false): Paint =
        Paint(Paint.ANTI_ALIAS_FLAG).apply {
            this.textSize = size
            this.color = color
            if (bold) typeface = android.graphics.Typeface.create(typeface, android.graphics.Typeface.BOLD)
        }

    private companion object {
        const val TAG = "CBCT_MEASURE_REPORT"
        val TIME_FMT = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
    }
}

/** VolumeInfo 的出生日期在 SR/PDF 里与检查日期同栏显示，缺失时给出占位 */
private fun VolumeInfo.studyDateEmptyAware(): String = patientBirthDate.ifEmpty { "-" }
