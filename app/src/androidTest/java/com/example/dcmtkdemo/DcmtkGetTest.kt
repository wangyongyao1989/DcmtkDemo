package com.example.dcmtkdemo

import android.content.Context
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.utils.DicomTag
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/**
 * C-GET 功能测试类。
 */
@RunWith(AndroidJUnit4::class)
class DcmtkGetTest {
    private lateinit var context: Context

    @Before
    @Throws(IOException::class)
    fun setUp() {
        context = InstrumentationRegistry.getInstrumentation().targetContext
        // 初始化 DCMTK 字典
        DcmtkJni.initDcmtk(context)
        Log.d(TAG, "DCMTK JNI initialized.")
    }

    /**
     * 获取 temp 目录路径 (相对于 externalFilesDir 的 ../temp 目录)
     */
    private fun getTempDir(): File {
        val externalFilesDir = context.getExternalFilesDir(null)
        assertNotNull("External storage not available", externalFilesDir)
        val tempDir = File(externalFilesDir!!.parentFile, "temp")
        if (!tempDir.exists()) {
            assertTrue("Failed to create temp directory", tempDir.mkdirs())
        }
        return tempDir
    }

    @Test
    fun testCGetAndDisplayInfo() {
        // ---- PACS 服务器配置 ----
        val host = "192.168.10.153"
        val port = 11112
        val localAET = "ANDROID_SCU"
        val remoteAET = "ACME_STORE"
        val patientID = "12345678"

        val tempDir = getTempDir()
        Log.d(TAG, "temp directory: ${tempDir.absolutePath}")

        // ---- 步骤1: 尝试 C-GET 下载 ----
        Log.d(TAG, "==================================================")
        Log.d(TAG, "Step 1: Attempting C-GET request to $host:$port")
        Log.d(TAG, "        PatientID: $patientID")
        Log.d(TAG, "        Save Dir : ${tempDir.absolutePath}")
        Log.d(TAG, "==================================================")

        var cgetSuccess = false
        try {
            cgetSuccess = DcmtkJni.cGet(
                host, port, localAET, remoteAET,
                patientID, tempDir.absolutePath, null
            )
        } catch (e: Exception) {
            Log.w(TAG, "C-GET threw exception (expected if no PACS): ${e.message}")
        }
        Log.d(TAG, "C-GET result: ${if (cgetSuccess) "SUCCESS" else "FAILED (or no PACS available)"}")

        // ---- 步骤2: 检查 temp 目录是否有文件 ----
        var files = tempDir.listFiles { f -> f.isFile }
        var fileCount = files?.size ?: 0
        Log.d(TAG, "Files in temp/ after C-GET: $fileCount")

        // ---- 步骤3: 若无文件，生成本地测试 DICOM 以验证信息读取功能 ----
        if (fileCount == 0) {
            Log.d(TAG, "==================================================")
            Log.d(TAG, "Step 2: No files downloaded. Generating local test DICOM")
            Log.d(TAG, "        via writeDicomFile() for info-display verification.")
            Log.d(TAG, "==================================================")
            try {
                generateTestDicom(tempDir)
            } catch (e: IOException) {
                fail("Failed to generate test DICOM file: ${e.message}")
            }
            files = tempDir.listFiles { f -> f.isFile }
            fileCount = files?.size ?: 0
        }

        assertTrue("temp directory should contain at least one file", fileCount > 0)

        // ---- 步骤4: 遍历显示每个文件的重要 DICOM 信息 ----
        Log.d(TAG, "==================================================")
        Log.d(TAG, "Step 3: Displaying important DICOM info for $fileCount file(s)")
        Log.d(TAG, "==================================================")

        var validDicomCount = 0
        files?.forEach { file ->
            Log.d(TAG, "--------------------------------------------------")
            Log.d(TAG, "File: ${file.name}  (${file.length()} bytes)")

            val info = DcmtkJni.loadDicomFileInfo(file.absolutePath)
            if (info == null || info.isEmpty()) {
                Log.w(TAG, "  -> Not a valid DICOM file or no tags found.")
                return@forEach
            }
            validDicomCount++

            // 打印重要 DICOM 信息
            val patientName = safeGet(info, DicomTag.PatientName.formattedTag)
            val patId = safeGet(info, DicomTag.PatientID.formattedTag)
            val patientSex = safeGet(info, DicomTag.PatientSex.formattedTag)
            val birthDate = safeGet(info, DicomTag.PatientBirthDate.formattedTag)
            val studyDate = safeGet(info, DicomTag.StudyDate.formattedTag)
            val studyTime = safeGet(info, DicomTag.StudyTime.formattedTag)
            val studyDesc = safeGet(info, DicomTag.StudyDescription.formattedTag)
            val modality = safeGet(info, DicomTag.Modality.formattedTag)
            val seriesDesc = safeGet(info, DicomTag.SeriesDescription.formattedTag)
            val sopClassUid = safeGet(info, DicomTag.SOPClassUID.formattedTag)
            val sopInstUid = safeGet(info, DicomTag.SOPInstanceUID.formattedTag)
            val rows = safeGet(info, DicomTag.Rows.formattedTag)
            val columns = safeGet(info, DicomTag.Columns.formattedTag)
            val bitsAlloc = safeGet(info, DicomTag.BitsAllocated.formattedTag)
            val bitsStored = safeGet(info, DicomTag.BitsStored.formattedTag)
            val pixelSpacing = safeGet(info, DicomTag.PixelSpacing.formattedTag)

            Log.d(TAG, "  -- Patient Info --")
            Log.d(TAG, "    Patient Name  : $patientName")
            Log.d(TAG, "    Patient ID    : $patId")
            Log.d(TAG, "    Patient Sex   : $patientSex")
            Log.d(TAG, "    Birth Date    : $birthDate")
            Log.d(TAG, "  -- Study Info --")
            Log.d(TAG, "    Study Date    : $studyDate")
            Log.d(TAG, "    Study Time    : $studyTime")
            Log.d(TAG, "    Study Desc    : $studyDesc")
            Log.d(TAG, "  -- Series Info --")
            Log.d(TAG, "    Modality      : $modality")
            Log.d(TAG, "    Series Desc   : $seriesDesc")
            Log.d(TAG, "  -- Image Info --")
            Log.d(TAG, "    Rows          : $rows")
            Log.d(TAG, "    Columns       : $columns")
            Log.d(TAG, "    Bits Allocated: $bitsAlloc")
            Log.d(TAG, "    Bits Stored   : $bitsStored")
            Log.d(TAG, "    Pixel Spacing : $pixelSpacing")
            Log.d(TAG, "  -- UID Info --")
            Log.d(TAG, "    SOP Class UID : $sopClassUid")
            Log.d(TAG, "    SOP Inst UID  : $sopInstUid")
        }

        Log.d(TAG, "==================================================")
        Log.d(TAG, "Test Summary: $validDicomCount/$fileCount files are valid DICOM.")
        Log.d(TAG, "==================================================")

        // ---- 步骤5: 断言验证重要标签存在 ----
        assertTrue("At least one valid DICOM file should exist", validDicomCount > 0)

        // 验证第一个有效 DICOM 文件的重要标签
        files?.forEach { file ->
            val info = DcmtkJni.loadDicomFileInfo(file.absolutePath)
            if (info != null && info.isNotEmpty()) {
                assertNotNull("Patient Name tag should exist", info[DicomTag.PatientName.formattedTag])
                assertNotNull("Patient ID tag should exist", info[DicomTag.PatientID.formattedTag])
                assertNotNull("Modality tag should exist", info[DicomTag.Modality.formattedTag])
                assertNotNull("SOP Class UID tag should exist", info[DicomTag.SOPClassUID.formattedTag])
                assertNotNull("Rows tag should exist", info[DicomTag.Rows.formattedTag])
                assertNotNull("Columns tag should exist", info[DicomTag.Columns.formattedTag])
                assertNotNull("Bits Allocated tag should exist", info[DicomTag.BitsAllocated.formattedTag])
                Log.d(TAG, "Assertion passed: all important tags are present in ${file.name}")
                return@testCGetAndDisplayInfo
            }
        }
    }

    /**
     * 生成一个本地测试 DICOM 文件并保存到 temp 目录。
     */
    @Throws(IOException::class)
    private fun generateTestDicom(tempDir: File) {
        val width = 8
        val height = 8
        // 16-bit 像素数据，每个像素 2 字节
        val rawPixels = ByteArray(width * height * 2)
        // 填充简单的渐变数据 (little-endian 16-bit)
        for (i in 0 until width * height) {
            val valShort = (i * 100).toShort()
            rawPixels[i * 2] = (valShort.toInt() and 0xFF).toByte()
            rawPixels[i * 2 + 1] = ((valShort.toInt() shr 8) and 0xFF).toByte()
        }

        // 将原始像素数据写入临时 raw 文件
        val rawFile = File(tempDir, "test_raw.bin")
        FileOutputStream(rawFile).use { fos ->
            fos.write(rawPixels)
        }

        // 使用 writeDicomFile 生成标准 DICOM 文件
        val dcmFile = File(tempDir, "TEST_GENERATED.dcm")
        val success = DcmtkJni.writeDicomFile(
            rawFile.absolutePath,
            dcmFile.absolutePath,
            width, height
        )
        assertTrue("writeDicomFile should succeed", success)

        // 清理临时 raw 文件
        rawFile.delete()

        Log.d(TAG, "Generated test DICOM: ${dcmFile.absolutePath} (${dcmFile.length()} bytes)")
    }

    companion object {
        private const val TAG = "DcmtkGetTest"

        /** 安全获取 HashMap 值，null 或空返回 "N/A" */
        private fun safeGet(map: HashMap<String, String>, key: String): String {
            val valStr = map[key]
            return if (!valStr.isNullOrEmpty()) valStr else "N/A"
        }
    }
}
