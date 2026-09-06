package com.wangyao.cbctdeal.jni

import android.graphics.Bitmap

/**
 * CBCT DICOM 序列解析 JNI 接口。
 *
 * 数据流（Native 堆零拷贝原则）：
 *  loadSeries -> Native 堆连续 Volume（返回指针）
 *  extract*   -> 按需从 Volume 提取切面并生成 Bitmap（一次一张）
 *  releaseVolume -> 释放 Native 内存
 *
 * 解析算法要点（详见模块 doc/ 下研究文档）：
 *  - 切片按 ImagePositionPatient[2]（Z 轴）排序，禁止按文件名排序；
 *  - 断层间隙自动补空白切片；缺失窗宽窗位时回退 CBCT 骨骼窗（WW4000/WC600）；
 *  - JPEG / JPEG-LS 压缩导出的序列自动解压；
 *  - 16bit 体素全程保留，仅在切面提取时做窗宽窗位映射。
 */
object CbctJni {

    init {
        System.loadLibrary("cbct_native")
    }

    /** 冠状面（固定 Y，横向 x 纵向 z） */
    const val PLANE_CORONAL = 0

    /** 矢状面（固定 X，横向 y 纵向 z） */
    const val PLANE_SAGITTAL = 1

    /**
     * 解析 CBCT DICOM 序列目录，组装 Native Volume。
     * @param dir 序列目录（应用可访问的文件系统路径）
     * @param callback 进度回调（[CbctProgressCallback]），可为 null
     * @return Volume Native 指针，0 表示失败
     */
    @JvmStatic
    external fun loadSeries(dir: String, callback: Any?): Long

    /** 读取 Volume 元数据（flat map，由 [com.wangyao.cbctdeal.model.CbctSeriesMeta] 组装） */
    @JvmStatic
    external fun getVolumeMeta(volumePtr: Long): HashMap<String, String>?

    /**
     * 提取横断面（Axial，固定 Z）。
     * @param sliceIndex 层索引 [0, depth)
     * @param ww 窗宽 @param wc 窗位（HU）
     */
    @JvmStatic
    external fun extractAxialSlice(
        volumePtr: Long, sliceIndex: Int, ww: Double, wc: Double
    ): Bitmap?

    /**
     * 提取 MPR 切面。
     * @param plane [PLANE_CORONAL] / [PLANE_SAGITTAL]
     * @param position 像素坐标（coronal: [0,height)，sagittal: [0,width)）
     */
    @JvmStatic
    external fun extractMpr(
        volumePtr: Long, plane: Int, position: Int, ww: Double, wc: Double
    ): Bitmap?

    /** 释放 Volume 的 Native 内存（释放后指针失效，禁止继续使用） */
    @JvmStatic
    external fun releaseVolume(volumePtr: Long)
}
