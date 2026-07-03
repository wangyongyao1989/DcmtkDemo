package com.example.dcmtk

import com.example.dcmtk.callback.MultiProgressCallback
import com.example.dcmtk.jni.DcmtkJni
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import android.util.Log

/**
 * 封装 PACS 操作的高层管理器
 * 1. 线程管理：强制在 Dispatchers.IO 中运行
 * 2. 异常重试：针对网络波动提供自动重试机制
 */
object PacsManager {
    private const val TAG = "PacsManager"

    /**
     * 带重试机制的 C-ECHO
     */
    @JvmStatic
    @JvmOverloads
    suspend fun safeCEcho(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        maxRetries: Int = 2
    ): Boolean = withContext(Dispatchers.IO) {
        var lastResult = false
        for (i in 0..maxRetries) {
            if (i > 0) {
                Log.w(TAG, "C-ECHO failed, retrying ($i/$maxRetries)...")
                delay(1000) // 重试前等待
            }
            lastResult = DcmtkJni.cEcho(host, port, localAET, remoteAET)
            if (lastResult) return@withContext true
        }
        lastResult
    }

    /**
     * 专门给 Java 调用的同步版本（需在后台线程运行）
     */
    @JvmStatic
    @JvmOverloads
    fun safeCEchoSync(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        maxRetries: Int = 2
    ): Boolean {
        var lastResult = false
        for (i in 0..maxRetries) {
            if (i > 0) {
                Log.w(TAG, "C-ECHO sync failed, retrying ($i/$maxRetries)...")
                try { Thread.sleep(1000); } catch (e: Exception) {}
            }
            lastResult = DcmtkJni.cEcho(host, port, localAET, remoteAET)
            if (lastResult) return true
        }
        return lastResult
    }

    /**
     * 在后台线程执行批量上传，并支持重试
     * 注意：cStoreMulti 内部已经处理了 Association 级别的复用
     */
    @JvmStatic
    @JvmOverloads
    suspend fun safeCStoreMulti(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        dcmPaths: Array<String>,
        callback: MultiProgressCallback,
        maxRetries: Int = 1
    ): Int = withContext(Dispatchers.IO) {
        var successCount = 0
        var attempt = 0
        
        // 简单的重试逻辑：如果一个都没成功，可能网络环境变了，重试一次
        while (attempt <= maxRetries) {
            if (attempt > 0) delay(2000)
            
            val result = DcmtkJni.cStoreMulti(host, port, localAET, remoteAET, dcmPaths, callback)
            if (result > 0 || dcmPaths.isEmpty()) {
                successCount = result
                break
            }
            attempt++
            Log.w(TAG, "cStoreMulti attempt $attempt failed to store any files.")
        }
        
        successCount
    }

    /**
     * 同步版本批量上传 (Java 友好)
     */
    @JvmStatic
    @JvmOverloads
    fun safeCStoreMultiSync(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        dcmPaths: Array<String>,
        callback: MultiProgressCallback,
        maxRetries: Int = 1
    ): Int {
        var successCount = 0
        var attempt = 0
        
        while (attempt <= maxRetries) {
            if (attempt > 0) try { Thread.sleep(2000); } catch (e: Exception) {}
            
            val result = DcmtkJni.cStoreMulti(host, port, localAET, remoteAET, dcmPaths, callback)
            if (result > 0 || dcmPaths.isEmpty()) {
                successCount = result
                break
            }
            attempt++
            Log.w(TAG, "cStoreMulti sync attempt $attempt failed.")
        }
        
        return successCount
    }

    /**
     * C-FIND 操作
     */
    @JvmStatic
    fun cFind(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        queryVal: String
    ): Array<String>? {
        return try {
            DcmtkJni.cFind(host, port, localAET, remoteAET, queryVal)
        } catch (e: Exception) {
            Log.e(TAG, "cFind failed", e)
            null
        }
    }

    /**
     * C-FIND By Accession
     */
    @JvmStatic
    fun cFindByAccession(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        accession: String
    ): Array<String>? {
        return try {
            DcmtkJni.cFindByAccession(host, port, localAET, remoteAET, accession)
        } catch (e: Exception) {
            Log.e(TAG, "cFindByAccession failed", e)
            null
        }
    }

    /**
     * C-GET 操作
     */
    @JvmStatic
    fun cGet(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        patId: String,
        saveDir: String,
        callback: com.example.dcmtk.callback.ProgressCallback
    ): Boolean {
        return try {
            DcmtkJni.cGet(host, port, localAET, remoteAET, patId, saveDir, callback)
        } catch (e: Exception) {
            Log.e(TAG, "cGet failed", e)
            false
        }
    }

    /**
     * MWL C-FIND 操作
     */
    @JvmStatic
    fun cFindMWL(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        modality: String
    ): Array<String>? {
        return try {
            DcmtkJni.cFindMWL(host, port, localAET, remoteAET, modality)
        } catch (e: Exception) {
            Log.e(TAG, "cFindMWL failed", e)
            null
        }
    }

    /**
     * 加载 DICOM 文件信息
     */
    @JvmStatic
    fun loadDicomFileInfo(path: String): java.util.HashMap<String, String>? {
        return try {
            DcmtkJni.loadDicomFileInfo(path)
        } catch (e: Exception) {
            Log.e(TAG, "loadDicomFileInfo failed", e)
            null
        }
    }
}
