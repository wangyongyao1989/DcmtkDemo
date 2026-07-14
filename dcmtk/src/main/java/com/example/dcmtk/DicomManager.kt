package com.example.dcmtk

import android.content.Context
import android.graphics.Bitmap
import android.util.Log
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.ScanRecord
import com.example.dcmtk.data.DicomWindowSettings
import com.example.dcmtk.model.PixelData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * 封装 DICOM 文件操作的高层管理器。
 *
 * 职责：
 * 1. 线程管理——所有 JNI 调用强制在 [Dispatchers.IO] 中执行，避免阻塞主线程；
 * 2. 异常隔离——统一捕获 native 层抛出的异常并返回 null，APP 层无需 try/catch；
 * 3. 接口收敛——APP 模块只依赖 [DicomManager]，不再直接引用 [DcmtkJni]。
 *
 * 对应 dcm4che3 版 DicomFileUtils.kt 中 loadDicomFileInfo / readDicomWindowSettings /
 * dicomFile2Bitmap / writeDcmFile 四个方法的 native 封装。
 */
object DicomManager {
    private const val TAG = "DicomManager"

    /**
     * 初始化 DCMTK 字典。必须在其他方法首次调用前执行一次。
     * @param context 用于从 assets 拷贝 dicom.dic 到内部存储
     */
    @JvmStatic
    fun init(context: Context) {
        DcmtkJni.initDcmtk(context)
    }

    /**
     * 加载 DICOM 文件命名信息（对应 dcm4che3 loadDicomFileInfo）。
     * 返回 10 个命名 key：PatientName / PatientBirthDate / InstitutionName / StationName /
     * Manufacturer / ManufacturerModelName / StudyDescription / SeriesDescription /
     * StudyDate(格式化为 YYYY-MM-DD HH:MM:SS) / PatientID / StudyID / PatientAge /
     * PatientSex(M→男/F→女) / BodyPartExamined / ExposureIndex。
     *
     * @param filePath DICOM 文件绝对路径
     * @return 命名 key 的信息 map；加载失败或异常返回 null
     */
    @JvmStatic
    suspend fun loadDicomFileInfoEx(filePath: String): HashMap<String, String>? =
        withContext(Dispatchers.IO) {
            try {
                DcmtkJni.loadDicomFileInfoEx(filePath)
            } catch (e: Exception) {
                Log.e(TAG, "loadDicomFileInfoEx failed: $filePath", e)
                null
            }
        }

    /**
     * 读取 DICOM 窗宽窗位及像素范围信息（对应 dcm4che3 readDicomWindowSettings）。
     * native 层返回扁平 map，由此处组装为 [DicomWindowSettings]。
     *
     * @param filePath DICOM 文件绝对路径
     * @return 组装后的 [DicomWindowSettings]；异常返回 null
     */
    @JvmStatic
    suspend fun readDicomWindowSettings(filePath: String): DicomWindowSettings? =
        withContext(Dispatchers.IO) {
            try {
                DcmtkJni.readDicomWindowSettings(filePath)
            } catch (e: Exception) {
                Log.e(TAG, "readDicomWindowSettings failed: $filePath", e)
                null
            }
        }

    /**
     * 读取 DICOM 窗宽窗位原生扁平 map（未经 Kotlin 组装）。
     * key 包括：smallestPixelValue / largestPixelValue / autoCenter / autoWidth /
     * windowCount / window_{i}_center / window_{i}_width / window_{i}_desc。
     *
     * 需要原始数据或自定义组装逻辑时使用；一般场景建议用 [readDicomWindowSettings]。
     *
     * @param filePath DICOM 文件绝对路径
     * @return 扁平 map；异常返回 null
     */
    @JvmStatic
    suspend fun readDicomWindowSettingsNative(filePath: String): HashMap<String, String>? =
        withContext(Dispatchers.IO) {
            try {
                DcmtkJni.readDicomWindowSettingsNative(filePath)
            } catch (e: Exception) {
                Log.e(TAG, "readDicomWindowSettingsNative failed: $filePath", e)
                null
            }
        }

    /**
     * 使用文件自带窗宽窗位渲染 DICOM 为 Bitmap（对应 dcm4che3 dicomFile2Bitmap(file)）。
     * 输出统一为 ARGB_8888 的 8-bit 灰度图。
     *
     * @param filePath DICOM 文件绝对路径
     * @return 渲染后的 Bitmap；失败返回 null
     */
    @JvmStatic
    suspend fun dicomFile2Bitmap(filePath: String): Bitmap? =
        withContext(Dispatchers.IO) {
            try {
                DcmtkJni.dicomFile2Bitmap(filePath)
            } catch (e: Exception) {
                Log.e(TAG, "dicomFile2Bitmap failed: $filePath", e)
                null
            }
        }

    /**
     * 使用自定义窗宽窗位渲染 DICOM 为 Bitmap（对应 dcm4che3 dicomFile2Bitmap(file, ww, wc)）。
     * 输出统一为 ARGB_8888 的 8-bit 灰度图。
     *
     * @param filePath      DICOM 文件绝对路径
     * @param windowWidth   窗宽
     * @param windowCenter  窗位
     * @return 渲染后的 Bitmap；失败返回 null
     */
    @JvmStatic
    suspend fun dicomFile2Bitmap(
        filePath: String,
        windowWidth: Double,
        windowCenter: Double
    ): Bitmap? = withContext(Dispatchers.IO) {
        try {
            DcmtkJni.dicomFile2BitmapWW(filePath, windowWidth, windowCenter)
        } catch (e: Exception) {
            Log.e(TAG, "dicomFile2Bitmap(ww,wc) failed: $filePath", e)
            null
        }
    }

    /**
     * 由 [ScanRecord] 与 [PixelData] 结构写出完整 CR DICOM（对应 dcm4che3 writeDcmFile）。
     *
     * 写入 tag 集合：PatientID/PatientName/PatientAge/PatientSex/InstitutionName/
     * Manufacturer/ManufacturerModelName/StudyDate/StudyTime/ToothPosition 等；
     * SOPClassUID 为 CR Image Storage，SOPInstanceUID 由 native 自动生成；
     * 输出传输语义为 Little Endian Explicit。
     *
     * @param record      检查记录（患者信息等元数据）
     * @param pixelData   像素数据处理结果（含 rows/columns/data/win_width/win_center 等）
     * @param dcmPath     输出 .dcm 文件路径
     * @return 成功返回 true
     */
    @JvmStatic
    suspend fun writeDcmFile(
        record: ScanRecord,
        pixelData: PixelData,
        dcmPath: String
    ): Boolean = withContext(Dispatchers.IO) {
        try {
            DcmtkJni.writeDcmFile(record, pixelData, dcmPath)
        } catch (e: Exception) {
            Log.e(TAG, "writeDcmFile failed: $dcmPath", e)
            false
        }
    }
}
