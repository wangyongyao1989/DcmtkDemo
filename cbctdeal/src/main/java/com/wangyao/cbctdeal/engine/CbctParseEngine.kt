package com.wangyao.cbctdeal.engine

import android.util.Log
import com.wangyao.cbctdeal.callback.CbctProgressCallback
import com.wangyao.cbctdeal.jni.CbctJni
import com.wangyao.cbctdeal.model.CbctSeriesMeta
import com.wangyao.cbctdeal.model.CbctVolumeHandle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * CBCT 序列解析引擎：对 [CbctJni] 的协程封装，
 * 供 UI 层在后台线程完成「目录 -> Volume 句柄」的完整解析。
 *
 * @param onProgress 进度回调（Native 工作线程回调，勿直接操作 UI）
 */
object CbctParseEngine {

    private const val TAG = "CbctParseEngine"

    /**
     * 解析序列目录。
     * @return Volume 句柄（内部持有 Native 指针与元数据），失败返回 null
     */
    suspend fun parse(
        dir: File,
        onProgress: (current: Int, total: Int) -> Unit = { _, _ -> },
    ): CbctVolumeHandle? = withContext(Dispatchers.IO) {
        val cb = object : CbctProgressCallback {
            override fun onProgress(current: Long, total: Long) {
                onProgress(current.toInt(), total.toInt())
            }
        }
        val ptr = try {
            CbctJni.loadSeries(dir.absolutePath, cb)
        } catch (e: Exception) {
            Log.e(TAG, "loadSeries exception", e)
            0L
        }
        if (ptr == 0L) {
            null
        } else {
            val map = CbctJni.getVolumeMeta(ptr)
            if (map == null) {
                CbctJni.releaseVolume(ptr)
                null
            } else {
                CbctVolumeHandle(ptr, CbctSeriesMeta.fromMap(map))
            }
        }
    }
}
