package com.wangyao.cbctmeasure.jni

import com.wangyao.cbctmeasure.model.RoiItem
import org.json.JSONObject

/**
 * ROI JNI（PRD 附录 B 的 RoiJni，功能项 R-01 ~ R-05）。
 *
 * ROI 与测量项一样以 JSON 双向传递，字段名单点来自
 * MeasurementManager::roiToJson / roiFromJson。
 */
object RoiJni {

    init {
        MeasureNative.ensureLoaded()
    }

    /** 新建 ROI，返回 id（<=0 失败） */
    @JvmStatic
    external fun addRoi(handle: Long, roiJson: String): Int

    /** 全量更新（缺失字段会被 Native 侧默认值覆盖，UI 编辑请优先用 roiPatch） */
    @JvmStatic
    external fun updateRoi(handle: Long, roiJson: String): Boolean

    /**
     * 浅合并更新：只改 patchJson 里出现的字段。
     * 滑杆 / 数值框的实时编辑走这条路径，避免"改了 huMin 把 polygon 清掉"。
     */
    @JvmStatic
    external fun roiPatch(handle: Long, id: Int, patchJson: String): Boolean

    @JvmStatic
    external fun removeRoi(handle: Long, id: Int): Boolean

    @JvmStatic
    external fun setRoiVisible(handle: Long, id: Int, visible: Boolean): Boolean

    /** ROI 量化统计（体积 / 面积 / HU 分布），返回 RoiStats JSON */
    @JvmStatic
    external fun statRoi(handle: Long, id: Int): String

    /** 主阈值 ROI 的 [lo,hi]，供 :cbctdeal 的"隔离显示"设置不透明度区间；无阈值 ROI 返回 null */
    @JvmStatic
    external fun dominantHuRange(handle: Long): DoubleArray?

    /** ROI 数组 JSON（UI 列表用；持久化走 MeasureJni.dumpRecords） */
    @JvmStatic
    external fun dumpRois(handle: Long): String

    // ---- 便捷包装 ----

    fun addRoi(handle: Long, roi: RoiItem): Int = addRoi(handle, roi.toJson().toString())

    fun updateRoi(handle: Long, roi: RoiItem): Boolean = updateRoi(handle, roi.toJson().toString())

    fun patch(handle: Long, id: Int, modify: JSONObject.() -> Unit): Boolean =
        roiPatch(handle, id, JSONObject().apply(modify).toString())

    fun list(handle: Long): List<RoiItem> = RoiItem.array(dumpRois(handle))
}
