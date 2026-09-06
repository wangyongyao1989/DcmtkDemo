package com.wangyao.cbctdeal.callback

/**
 * CBCT 序列解析进度回调。
 *
 * 注意：回调来自 Native 解析线程池中的工作线程，不保证在主线程，
 * 调用方如需更新 UI 必须自行切换到主线程。
 */
interface CbctProgressCallback {
    /** @param current 已完成切片数 @param total 总步骤数（两阶段：元数据 + 像素） */
    fun onProgress(current: Long, total: Long)
}
