package com.wangyao.cbctmeasure.jni

import com.wangyao.cbctmeasure.model.SrResult
import org.json.JSONObject

/**
 * 报告导出 JNI（PRD 附录 B 的 ReportJni，功能项 5.5 / 验收 AC-09）。
 *
 * 只把 DICOM SR 放 Native：SR 是 DCMTK dcmsr 的强项，自己拼数据集不现实。
 * PDF 不走 JNI —— Android 侧用 android.graphics.pdf.PdfDocument 排版，
 * 图像证据用 CbctVtkJni.captureFrame() 取帧，两者都不需要 DCMTK。
 *
 * 字典注入：本模块的 dcmDataDict 是 libcbct_measure.so 内的独立副本
 * （不与 libcbct_native.so 共享），所以必须自己注入一次 dicom.dic，
 * 路径复用 :cbctdeal 的 CbctParseEngine 已经拷到 filesDir 的那份文件。
 */
object ReportJni {

    init {
        MeasureNative.ensureLoaded()
    }

    /**
     * 注入 DCMTK 数据字典。
     * @return 空串表示成功，否则为错误文本（可直接展示给医生）
     */
    @JvmStatic
    external fun initSrDictionary(dictPath: String): String

    /**
     * 生成 Comprehensive SR 并落盘。
     * 测量项、种植体、神经管全部由 Native 从会话直接取，
     * Kotlin 只负责传报告级的文字信息（操作者、描述、可覆写的患者字段）。
     * @return {"ok":bool,"sopUid":"...","error":"..."}
     */
    @JvmStatic
    external fun exportSr(handle: Long, requestJson: String, outPath: String): String

    /**
     * 读取并严格解析已生成的 SR（AC-09 的现场自校验，等价于跑一次 dsr2xml）。
     * @return {"ok":bool,"docType":"...","summary":"...","error":"..."}
     */
    @JvmStatic
    external fun verifySr(path: String): String

    /** 新建 SOP Instance UID（归档 / C-STORE 引用用） */
    @JvmStatic
    external fun newSopUid(): String

    // ---- 便捷包装 ----

    /**
     * 组装 SR 请求。未在此处设置的字段由 Native 用体数据元数据补齐
     * （患者姓名/编号/性别/出生日期、Study/Series UID、设备机构名等）。
     */
    fun request(
        operatorName: String,
        description: String,
        modify: (JSONObject.() -> Unit)? = null,
    ): String = JSONObject().apply {
        put("operatorName", operatorName)
        put("description", description)
        modify?.invoke(this)
    }.toString()

    /** 导出并解析回执（external 版返回 JSON 文本，这里给出结构化结果） */
    fun export(handle: Long, requestJson: String, outPath: String): SrResult =
        SrResult.from(exportSr(handle, requestJson, outPath))

    /** 自校验并解析回执 */
    fun check(path: String): SrResult = SrResult.from(verifySr(path))
}
