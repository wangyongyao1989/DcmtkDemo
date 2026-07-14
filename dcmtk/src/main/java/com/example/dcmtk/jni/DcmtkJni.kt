package com.example.dcmtk.jni

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import com.example.dcmtk.callback.MultiProgressCallback
import com.example.dcmtk.callback.ProgressCallback
import com.example.dcmtk.model.ScanRecord
import com.example.dcmtk.utils.DcmtkFileUtil
import com.example.dcmtk.data.DicomWindowSettings
import com.example.dcmtk.model.PixelData
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

    // =========================================================================
    // 对应 dcm4che3 版 DicomFileUtils.kt 的新增方法（DCMTK native 实现）
    // =========================================================================

    /** 对应 loadDicomFileInfo(file)：返回命名 key 的信息 map */
    @JvmStatic
    external fun loadDicomFileInfoEx(filePath: String): HashMap<String, String>?

    /**
     * 对应 readDicomWindowSettings(file)：native 返回扁平 map，由此处组装为
     * [DicomWindowSettings]，避免在 JNI 中构造嵌套 List。
     */
    @JvmStatic
    fun readDicomWindowSettings(filePath: String): DicomWindowSettings {
        val m = readDicomWindowSettingsNative(filePath) ?: HashMap()
        val smallest = m["smallestPixelValue"]?.toIntOrNull() ?: 0
        val largest = m["largestPixelValue"]?.toIntOrNull() ?: 4095
        val autoCenter = m["autoCenter"]?.toDoubleOrNull() ?: (smallest + largest) / 2.0
        val autoWidth = m["autoWidth"]?.toDoubleOrNull() ?: (largest - smallest).toDouble()
        val count = m["windowCount"]?.toIntOrNull() ?: 0
        val windows = ArrayList<DicomWindowSettings.DicomWindow>()
        for (i in 0 until count) {
            val c = m["window_${i}_center"]?.toDoubleOrNull() ?: continue
            val w = m["window_${i}_width"]?.toDoubleOrNull() ?: continue
            val d = m["window_${i}_desc"]?.let { if (it.isEmpty()) null else it }
            windows.add(DicomWindowSettings.DicomWindow(c, w, d))
        }
        return DicomWindowSettings(
            smallestPixelValue = smallest,
            largestPixelValue = largest,
            windows = windows,
            autoCalculatedWindow = DicomWindowSettings.DicomWindow(autoCenter, autoWidth, "自动计算")
        )
    }

    @JvmStatic
    external fun readDicomWindowSettingsNative(filePath: String): HashMap<String, String>?

    /** 对应 dicomFile2Bitmap(file)：使用文件自带窗宽窗位渲染 */
    @JvmStatic
    external fun dicomFile2Bitmap(filePath: String): Bitmap?

    /** 对应 dicomFile2Bitmap(file, windowWidth, windowCenter)：使用自定义窗宽窗位渲染 */
    @JvmStatic
    external fun dicomFile2BitmapWW(filePath: String, windowWidth: Double, windowCenter: Double): Bitmap?

    /**
     * 对应 writeDcmFile(record, rawFile, dcmFile, imageWidth, imageHeight)：
     * 由 [ScanRecord] 与 [PixelData] 像素结构写出完整 CR DICOM。
     */
    @JvmStatic
    external fun writeDcmFile(
        record: ScanRecord,
        pixelData: PixelData,
        dcmPath: String
    ): Boolean
}
