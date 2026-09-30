package com.wangyao.cbctmeasure.jni

import com.wangyao.cbctmeasure.model.AnnotationItem
import org.json.JSONObject

/**
 * 标注 JNI（PRD 附录 B 的 AnnotationJni，功能项 A-01 ~ A-07）。
 *
 * 标注与测量共用同一个会话 handle：AnnotationStore 是 MeasureSession 的成员，
 * 因此"删除测量项"不会连带删除绑定它的人工批注（measureId=0 表示独立标注），
 * 但会话销毁时二者一起释放。
 */
object AnnotationJni {

    init {
        MeasureNative.ensureLoaded()
    }

    /** 新建标注，返回 id（<=0 表示入参非法） */
    @JvmStatic
    external fun addAnnotation(handle: Long, annoJson: String): Int

    @JvmStatic
    external fun updateAnnotation(handle: Long, annoJson: String): Boolean

    @JvmStatic
    external fun removeAnnotation(handle: Long, id: Int): Boolean

    /** A-01 文字内容的快捷编辑（列表中双击即可改） */
    @JvmStatic
    external fun setAnnotationText(handle: Long, id: Int, text: String): Boolean

    @JvmStatic
    external fun setAnnotationColor(handle: Long, id: Int, color: Int): Boolean

    @JvmStatic
    external fun setAnnotationVisible(handle: Long, id: Int, visible: Boolean): Boolean

    /** 标注归档 JSON（PRD 8.4 的 *_anno.json 内容） */
    @JvmStatic
    external fun dumpAnnotations(handle: Long): String

    @JvmStatic
    external fun loadAnnotations(handle: Long, json: String): Boolean

    @JvmStatic
    external fun clearAnnotations(handle: Long)

    // ---- 便捷包装 ----

    fun addAnnotation(handle: Long, anno: AnnotationItem): Int =
        addAnnotation(handle, anno.toJson().toString())

    fun updateAnnotation(handle: Long, anno: AnnotationItem): Boolean =
        updateAnnotation(handle, anno.toJson().toString())

    /** 解析 dumpAnnotations() 文本，取标注列表 */
    fun list(handle: Long): List<AnnotationItem> = try {
        AnnotationItem.fromRoot(JSONObject(dumpAnnotations(handle)))
    } catch (e: Exception) {
        emptyList()
    }
}
