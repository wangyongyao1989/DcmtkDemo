package com.example.dcmtk.jni

import android.content.Context
import android.util.Log
import com.example.dcmtk.callback.MultiProgressCallback
import com.example.dcmtk.callback.ProgressCallback
import com.example.dcmtk.utils.DcmtkFileUtil
import java.io.IOException

object DcmtkJni {

    init {
        System.loadLibrary("dcmtk_native")
    }

    external fun stringFromJNI(): String

    /**
     * 初始化 DCMTK 字典
     * @param context 上下文，用于从 assets 中拷贝 dicom.dic
     */
    @JvmStatic
    fun initDcmtk(context: Context) {
        try {
            val dictPath = DcmtkFileUtil.copyAssetToInternalStorage(context, "dicom.dic")
            initDcmtk(dictPath)
        } catch (e: IOException) {
            Log.e("DcmtkJni", "Failed to init dcmtk dictionary", e)
        }
    }

    @JvmStatic
    external fun initDcmtk(dictPath: String)

    @JvmStatic
    external fun loadDicomFileInfo(filePath: String): HashMap<String, String>?

    @JvmStatic
    external fun writeDicomFile(rawDataPath: String, destDcmPath: String, width: Int, height: Int): Boolean

    @JvmStatic
    external fun connectPACS(host: String, port: Int, localAET: String, remoteAET: String): Boolean

    @JvmStatic
    external fun cEcho(host: String, port: Int, localAET: String, remoteAET: String): Boolean

    @JvmStatic
    external fun cStore(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        dcmPath: String,
        callback: ProgressCallback?
    ): Boolean

    @JvmStatic
    external fun cStoreMulti(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        dcmPaths: Array<String>,
        callback: MultiProgressCallback?
    ): Int

    @JvmStatic
    external fun cFind(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        patientName: String
    ): Array<String>?

    @JvmStatic
    external fun cFindByAccession(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        accessionNumber: String
    ): Array<String>?

    @JvmStatic
    external fun cFindMWL(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        modality: String
    ): Array<HashMap<String, String>>?

    @JvmStatic
    external fun cFindMWLByTemplate(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        templatePath: String,
        outputDir: String
    ): Array<String>?

    @JvmStatic
    external fun cMove(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        patientID: String,
        destAET: String
    ): Boolean

    @JvmStatic
    external fun cGet(
        host: String,
        port: Int,
        localAET: String,
        remoteAET: String,
        patientID: String,
        saveDir: String,
        callback: ProgressCallback?
    ): Boolean

    @JvmStatic
    external fun dcmToJpg(dir: String): Int

    @JvmStatic
    external fun cancelOperation()
}
