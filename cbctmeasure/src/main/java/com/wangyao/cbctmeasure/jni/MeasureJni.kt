package com.wangyao.cbctmeasure.jni

import com.wangyao.cbctmeasure.model.WorldPoint

/**
 * 测量会话 JNI（PRD 附录 B 的 MeasureJni）。
 *
 * 三层结构里的"桥接层"：本文件只有 external 声明与少量便捷包装，
 * 计算全在 core/，渲染全在 :cbctdeal，持久化全在 store/。
 *
 * 会话模型：一个 handle 对应 Native 侧一个 MeasureSession
 * （MeasurementManager + AnnotationStore + 叠加层缓存）。
 * 生命周期铁律与 :cbctdeal 一致 ——
 *   destroySession(handle) 必须先于 CbctJni.releaseVolume(volumePtr)，
 * 否则会话里的 VolumeRef 会变成悬垂指针（Native 不接管体数据所有权）。
 */
object MeasureJni {

    init {
        MeasureNative.ensureLoaded()
    }

    /** 创建会话并绑定体数据；volumePtr 来自 CbctJni.loadSeries()，允许传 0 */
    @JvmStatic
    external fun createSession(volumePtr: Long): Long

    /** 销毁会话（释放 Native 内存；必须早于 releaseVolume 调用） */
    @JvmStatic
    external fun destroySession(handle: Long)

    /** 重新解析序列后换绑体数据，内部会重算 ROI 统计与种植体安全 */
    @JvmStatic
    external fun bindVolume(handle: Long, volumePtr: Long): Boolean

    /**
     * 追加一条测量。
     * @param points 扁平世界坐标（3*n，毫米）
     * @param roiId M-04 / M-05 / M-08 关联的 ROI，0 表示无
     * @return 新记录 id，<=0 表示失败
     */
    @JvmStatic
    external fun addMeasure(
        handle: Long, type: Int, points: DoubleArray, roiId: Int,
        name: String, color: Int, note: String
    ): Int

    @JvmStatic
    external fun recalcMeasure(handle: Long, id: Int): Boolean

    @JvmStatic
    external fun removeMeasure(handle: Long, id: Int): Boolean

    @JvmStatic
    external fun renameMeasure(handle: Long, id: Int, name: String): Boolean

    @JvmStatic
    external fun setMeasureNote(handle: Long, id: Int, note: String): Boolean

    @JvmStatic
    external fun setMeasureVisible(handle: Long, id: Int, visible: Boolean): Boolean

    @JvmStatic
    external fun clearMeasures(handle: Long)

    /** 测量 + ROI 的归档 JSON 文本（PRD 8.4 的 *_measure.json 内容） */
    @JvmStatic
    external fun dumpRecords(handle: Long): String

    /** 种植体 + 神经管方案的归档 JSON 文本（PRD 8.4 的 *_plan.json 内容） */
    @JvmStatic
    external fun dumpPlan(handle: Long): String

    @JvmStatic
    external fun restoreRecords(handle: Long, json: String): Boolean

    @JvmStatic
    external fun restorePlan(handle: Long, json: String): Boolean

    /** 体数据概况 JSON（尺寸/体素/序列 UID/患者信息） */
    @JvmStatic
    external fun dumpVolumeInfo(handle: Long): String

    /** 会话摘要一行文本（UI 顶部状态栏） */
    @JvmStatic
    external fun summaryText(handle: Long): String

    /**
     * 射线拾取（PRD 5.1.3 点按取点）。
     * reqJson 字段见 core/MeasurePicker.h 的 PickRequest：
     * {"ray":[6 个 double，来自 CbctVtkJni.displayToRay],"mode":0..5,
     *  "huThreshold","skip","lo","hi","plane","position","roiId","nerveId","toleranceMm"}
     */
    @JvmStatic
    external fun pick(handle: Long, reqJson: String): String

    /** 吸附到最近体素中心（落点消抖），返回 3 个 double */
    @JvmStatic
    external fun snapToVoxel(handle: Long, x: Double, y: Double, z: Double): DoubleArray?

    /**
     * 叠加层图元描述 JSON。
     * from/count 的下标基准是 overlayPoints()，两者必须在同一帧配套取用。
     * @param includePixel 是否输出 A-07 截图标注（像素坐标，仅报告合成时为 true）
     */
    @JvmStatic
    external fun overlayJson(handle: Long, plane: Int, position: Int, includePixel: Boolean): String

    /** 叠加层全部顶点（世界 mm，扁平 3*n），整批交给 CbctVtkView.projectPoints */
    @JvmStatic
    external fun overlayPoints(handle: Long): DoubleArray

    /** 叠加层内容版本号：数据未变化时保持不变，Kotlin 端据此跳过重复投影 */
    @JvmStatic
    external fun overlayVersion(handle: Long): Long

    /** HU -> 组织类型名（M-07；纯函数，不需要会话） */
    @JvmStatic
    external fun tissueName(hu: Double): String

    // ---- 便捷包装：把"扁平数组 / JSON 文本"这层协议挡在 model 之外 ----

    /** 解析 dumpRecords() 的测量列表（UI 结果表 / 状态机提交后回读读数） */
    fun records(handle: Long): List<com.wangyao.cbctmeasure.model.MeasureItem> = try {
        com.wangyao.cbctmeasure.model.MeasureItem.list(
            org.json.JSONObject(dumpRecords(handle))
        )
    } catch (e: Exception) {
        emptyList()
    }

    /** 解析 dumpVolumeInfo()：报告页眉、归档文件名、SR 患者信息都以此为准 */
    fun volumeInfo(handle: Long): com.wangyao.cbctmeasure.model.VolumeInfo =
        if (handle == 0L) com.wangyao.cbctmeasure.model.VolumeInfo.INVALID else try {
            com.wangyao.cbctmeasure.model.VolumeInfo.from(org.json.JSONObject(dumpVolumeInfo(handle)))
        } catch (e: Exception) {
            com.wangyao.cbctmeasure.model.VolumeInfo.INVALID
        }

    /** List<WorldPoint> -> 扁平 doubleArray（调用方不必每处手写 3*n 展开） */
    fun flatten(points: List<WorldPoint>): DoubleArray {
        val out = DoubleArray(points.size * 3)
        for ((i, p) in points.withIndex()) {
            out[i * 3] = p.x
            out[i * 3 + 1] = p.y
            out[i * 3 + 2] = p.z
        }
        return out
    }

    /** 扁平 doubleArray -> List<WorldPoint> */
    fun unflatten(flat: DoubleArray?): List<WorldPoint> {
        if (flat == null || flat.size < 3) return emptyList()
        val out = ArrayList<WorldPoint>(flat.size / 3)
        var i = 0
        while (i + 2 < flat.size) {
            out.add(WorldPoint(flat[i], flat[i + 1], flat[i + 2]))
            i += 3
        }
        return out
    }
}
