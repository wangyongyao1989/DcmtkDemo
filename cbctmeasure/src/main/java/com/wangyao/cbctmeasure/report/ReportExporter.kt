package com.wangyao.cbctmeasure.report

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import com.wangyao.cbctmeasure.jni.MeasureJni
import com.wangyao.cbctmeasure.jni.ReportJni
import com.wangyao.cbctmeasure.model.SrResult
import com.wangyao.cbctmeasure.model.VolumeInfo
import com.wangyao.cbctmeasure.store.MeasurementStore
import com.wangyao.cbctmeasure.store.StorePaths
import java.io.File

/**
 * 一键导出编排（PRD 5.5：报告 PDF + DICOM SR；PRD 8.4：归档与截图目录）。
 *
 * 顺序是有讲究的：
 *   1) 先存测量/标注/方案归档 —— 报告与 SR 都要引用同一份数据；
 *   2) 再截渲染帧（此刻叠加图形与屏幕所见一致）；
 *   3) 生成 PDF（内部会重新从会话取数，保证与截图同源）；
 *   4) 导出 SR 并**现场用 verifySr 读回自校验**（AC-09 不依赖 PC 侧 dsr2xml）。
 * 任何一步失败都只记录到自己的字段里，不把整条链打断 ——
 * 医生拿到 PDF 但 SR 失败，比"整个导出报错、什么都没拿到"更有用。
 */
class ReportExporter(private val context: Context) {

    /** 导出总览：UI 直接逐行展示 */
    data class Summary(
        val key: String,
        val archive: List<MeasurementStore.Result>,
        val screenshot: String,
        val pdf: ReportGenerator.Output,
        val sr: SrResult,
        val srPath: String,
        val srVerified: SrResult,
    ) {
        fun allOk(): Boolean = pdf.ok && sr.ok && srVerified.ok && archive.all { it.ok }

        fun lines(): List<String> = buildList {
            add("归档：${archive.joinToString("；") { it.message }}")
            add("截图：$screenshot")
            add("PDF：${pdf.message}（${pdf.file.absolutePath}）")
            add("SR：${if (sr.ok) "已导出 ${sr.sopUid}" else sr.error}（$srPath）")
            add("SR 自校验：${if (srVerified.ok) "通过 ${srVerified.docType} ${srVerified.summary}" else srVerified.error}")
        }
    }

    /**
     * @param handle 会话句柄
     * @param info 体数据概况（StudyUID/SeriesUID 决定归档文件名，PRD 8.4）
     * @param evidence 当前渲染帧（含叠加图形）
     * @param operatorName 报告操作者（写入 PDF 页眉与 SR）
     */
    fun exportAll(
        handle: Long,
        info: VolumeInfo,
        evidence: Bitmap?,
        operatorName: String,
    ): Summary {
        val key = info.archiveKey()
        val archive = MeasurementStore.saveAll(context, handle, key)

        var screenshotText = "无渲染帧"
        if (evidence != null) {
            val shot = MeasurementStore.saveScreenshot(context, info.studyInstanceUID, evidence)
            screenshotText = if (shot.ok) shot.file.absolutePath else shot.message
        }

        val pdf = ReportGenerator(context).generate(handle, info, evidence, operatorName)

        val srFile = StorePaths.srFile(context, info.studyInstanceUID, System.currentTimeMillis())
        val request = ReportJni.request(operatorName, "CBCT 测量与手术规划报告")
        val sr = exportSrSafe(handle, request, srFile)
        val verify = if (sr.ok) verifySafe(srFile) else SrResult(false, "", "", "", "SR 未生成，跳过校验")

        Log.i(
            TAG, "exportAll key=$key archive=${archive.count { it.ok }}/${archive.size}" +
                    " pdf=${pdf.ok} pages=${pdf.pages} sr=${sr.ok} verify=${verify.ok}"
        )
        return Summary(key, archive, screenshotText, pdf, sr, srFile.absolutePath, verify)
    }

    /**
     * 数据字典注入（SR 的前置条件）。
     *
     * 本模块的 DCMTK 是独立静态副本，字典不会与 libcbct_native.so 共享，
     * 因此即使 :cbctdeal 已经注入过一次，这里仍要按本 .so 再注入一次；
     * 复用 :cbctdeal 释放到 filesDir 的 dicom.dic，避免在 assets 里放第二份。
     * @return 空串表示成功，否则为可直接展示的中文错误
     */
    fun ensureDictionary(): String {
        val dic = File(context.filesDir, "dicom.dic")
        if (!dic.exists()) {
            Log.e(TAG, "dicom.dic not found: ${dic.absolutePath}")
            return "未找到数据字典 dicom.dic，请先在 CBCT Parse 页解析一次序列"
        }
        // 与 :cbctdeal 的 CbctParseEngine 一致：loadDictionary 收的是 dicom.dic 的文件全路径
        val err = runCatching { ReportJni.initSrDictionary(dic.absolutePath) }
            .getOrElse { "字典注入异常：${it.message}" }
        if (err.isNotEmpty()) Log.e(TAG, "initSrDictionary failed: $err")
        return err
    }

    /** 读回历史归档并灌进会话（PRD 5.6 的"下次进入继续编辑"） */
    fun restore(handle: Long, info: VolumeInfo): List<MeasurementStore.Result> =
        MeasurementStore.loadAll(context, handle, info.archiveKey())

    private fun exportSrSafe(handle: Long, request: String, out: File): SrResult {
        return try {
            out.parentFile?.mkdirs()
            ReportJni.export(handle, request, out.absolutePath)
        } catch (e: Exception) {
            Log.e(TAG, "exportSr failed", e)
            SrResult(false, "", "", "", "SR 导出异常：${e.message}")
        }
    }

    private fun verifySafe(path: File): SrResult = try {
        ReportJni.check(path.absolutePath)
    } catch (e: Exception) {
        Log.e(TAG, "verifySr failed", e)
        SrResult(false, "", "", "", "SR 校验异常：${e.message}")
    }

    /** 会话摘要（UI 顶部状态栏一行；直接取 Native 文本，避免两处统计口径） */
    fun sessionSummary(handle: Long): String =
        runCatching { MeasureJni.summaryText(handle) }.getOrDefault("")

    private companion object {
        const val TAG = "CBCT_MEASURE_REPORT"
    }
}
