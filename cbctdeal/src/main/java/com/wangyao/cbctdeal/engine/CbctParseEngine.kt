package com.wangyao.cbctdeal.engine

import android.content.Context
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

    /** 数据字典是否已注入 Native（进程级一次，避免重复加载） */
    @Volatile
    private var dictReady = false

    /**
     * 确保 Native 侧 DICOM 数据字典已加载。
     *
     * 交叉编译的 DCMTK 静态库以 --without-private-dictionary 构建，
     * 内置字典为空实现，JPEG / JPEG-LS 压缩序列解压时标准 tag 的
     * VR 查询会失败（"Tag not found in data dictionary"），
     * 故将模块 assets 内置的 dicom.dic 释放到内部存储后注入。
     */
    private fun ensureDictionary(context: Context) {
        if (dictReady) return
        val dictFile = File(context.filesDir, "dicom.dic")
        if (!dictFile.exists() || dictFile.length() == 0L) {
            context.assets.open("dicom.dic").use { input ->
                dictFile.outputStream().use { output -> input.copyTo(output) }
            }
        }
        CbctJni.initDictionary(dictFile.absolutePath)
        dictReady = true
        Log.i(TAG, "DICOM data dictionary injected: ${dictFile.absolutePath}")
    }

    /**
     * 解析序列目录（首次调用自动注入数据字典）。
     * @return Volume 句柄（内部持有 Native 指针与元数据），失败返回 null
     */
    suspend fun parse(
        context: Context,
        dir: File,
        onProgress: (current: Int, total: Int) -> Unit = { _, _ -> },
    ): CbctVolumeHandle? = withContext(Dispatchers.IO) {
        ensureDictionary(context)
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
