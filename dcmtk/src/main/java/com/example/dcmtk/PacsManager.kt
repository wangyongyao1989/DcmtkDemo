package com.example.dcmtk

import com.example.dcmtk.callback.MultiProgressCallback
import com.example.dcmtk.jni.DcmtkJni
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import android.util.Log

import com.example.dcmtk.model.PacsConfig

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
        config: PacsConfig,
        maxRetries: Int = 2
    ): Boolean = withContext(Dispatchers.IO) {
        var lastResult = false
        for (i in 0..maxRetries) {
            if (i > 0) {
                Log.w(TAG, "C-ECHO failed, retrying ($i/$maxRetries)...")
                delay(1000) // 重试前等待
            }
            lastResult = DcmtkJni.cEcho(config.host, config.port, config.localAet, config.remoteAet)
            if (lastResult) return@withContext true
        }
        lastResult
    }

    /**
     * 在后台线程执行批量上传，并支持重试
     * 注意：cStoreMulti 内部已经处理了 Association 级别的复用
     */
    @JvmStatic
    @JvmOverloads
    suspend fun safeCStoreMulti(
        config: PacsConfig,
        dcmPaths: Array<String>,
        callback: MultiProgressCallback,
        maxRetries: Int = 1
    ): Int = withContext(Dispatchers.IO) {
        var successCount = 0
        var attempt = 0
        
        // 简单的重试逻辑：如果一个都没成功，可能网络环境变了，重试一次
        while (attempt <= maxRetries) {
            if (attempt > 0) delay(2000)
            
            val result = DcmtkJni.cStoreMulti(config.host, config.port
                , config.localAet, config.remoteAet, dcmPaths, callback)
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
     * C-FIND 操作
     */
    @JvmStatic
    suspend fun cFind(
        config: PacsConfig,
        queryVal: String
    ): Array<String>? = withContext(Dispatchers.IO) {
        try {
            DcmtkJni.cFind(config.host, config.port, config.localAet
                , config.remoteAet, queryVal)
        } catch (e: Exception) {
            Log.e(TAG, "cFind failed", e)
            null
        }
    }

    /**
     * C-FIND By Accession
     */
    @JvmStatic
    suspend fun cFindByAccession(
        config: PacsConfig,
        accession: String
    ): Array<String>? = withContext(Dispatchers.IO) {
        try {
            DcmtkJni.cFindByAccession(config.host, config.port, config.localAet
                , config.remoteAet, accession)
        } catch (e: Exception) {
            Log.e(TAG, "cFindByAccession failed", e)
            null
        }
    }

    /**
     * C-GET 操作
     */
    @JvmStatic
    suspend fun cGet(
        config: PacsConfig,
        patId: String,
        saveDir: String,
        callback: com.example.dcmtk.callback.ProgressCallback
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            DcmtkJni.cGet(config.host, config.port, config.localAet, config.remoteAet
                , patId, saveDir, callback)
        } catch (e: Exception) {
            Log.e(TAG, "cGet failed", e)
            false
        }
    }

    /**
     * MWL C-FIND 操作
     */
    @JvmStatic
    suspend fun cFindMWL(
        config: PacsConfig,
        modality: String
    ): Array<HashMap<String, String>>? = withContext(Dispatchers.IO) {
        try {
            DcmtkJni.cFindMWL(config.host, config.port, config.localAet, config.remoteAet, modality)
        } catch (e: Exception) {
            Log.e(TAG, "cFindMWL failed", e)
            null
        }
    }

    /**
     * MWL C-FIND By Template 操作
     */
    @JvmStatic
    suspend fun cFindMWLByTemplate(
        config: PacsConfig,
        templatePath: String,
        outputDir: String
    ): Array<String>? = withContext(Dispatchers.IO) {
        try {
            DcmtkJni.cFindMWLByTemplate(
                config.host, config.port, config.localAet, config.remoteAet,
                templatePath, outputDir
            )
        } catch (e: Exception) {
            Log.e(TAG, "cFindMWLByTemplate failed", e)
            null
        }
    }

    /**
     * 加载 DICOM 文件信息
     */
    @JvmStatic
    suspend fun loadDicomFileInfo(path: String): java.util.HashMap<String, String>? = withContext(Dispatchers.IO) {
        try {
            DcmtkJni.loadDicomFileInfo(path)
        } catch (e: Exception) {
            Log.e(TAG, "loadDicomFileInfo failed", e)
            null
        }
    }

    /**
     * 取消当前正在进行的 PACS 网络操作
     */
    @JvmStatic
    fun cancelOperation() {
        Log.i(TAG, "cancelOperation requested.")
        DcmtkJni.cancelOperation()
    }
}
