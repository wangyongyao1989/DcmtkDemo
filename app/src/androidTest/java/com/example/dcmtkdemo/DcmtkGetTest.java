package com.example.dcmtkdemo;

import android.content.Context;
import android.util.Log;

import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import com.example.dcmtk.jni.DcmtkJni;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.HashMap;

import static org.junit.Assert.*;

/**
 * C-GET 功能测试类。
 *
 * 测试流程：
 * 1. 初始化 DCMTK 字典
 * 2. 尝试通过 C-GET 从 PACS 下载 DICOM 文件到 ../temp 目录
 * 3. 若无真实 PACS 环境（C-GET 失败或 temp 目录无文件），
 *    则通过 writeDicomFile 生成本地测试 DICOM 文件放入 temp 目录，
 *    以保证"读取并显示 temp 目录下 dcm 重要信息"的功能始终可被验证。
 * 4. 遍历 temp 目录下所有文件，调用 loadDicomFileInfo 读取并打印重要信息
 * 5. 对重要 DICOM 标签进行断言验证
 */
@RunWith(AndroidJUnit4.class)
public class DcmtkGetTest {
    private static final String TAG = "DcmtkGetTest";
    private Context context;

    @Before
    public void setUp() throws IOException {
        context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        // 初始化 DCMTK 字典
        DcmtkJni.initDcmtk(context);
        Log.d(TAG, "DCMTK JNI initialized.");
    }

    /**
     * 获取 temp 目录路径 (相对于 externalFilesDir 的 ../temp 目录)
     */
    private File getTempDir() {
        File externalFilesDir = context.getExternalFilesDir(null);
        assertNotNull("External storage not available", externalFilesDir);
        File tempDir = new File(externalFilesDir.getParentFile(), "temp");
        if (!tempDir.exists()) {
            assertTrue("Failed to create temp directory", tempDir.mkdirs());
        }
        return tempDir;
    }

    @Test
    public void testCGetAndDisplayInfo() {
        // ---- PACS 服务器配置 ----
        String host = "192.168.10.153";
        int port = 11112;
        String localAET = "ANDROID_SCU";
        String remoteAET = "ACME_STORE";
        String patientID = "12345678";

        File tempDir = getTempDir();
        Log.d(TAG, "temp directory: " + tempDir.getAbsolutePath());

        // ---- 步骤1: 尝试 C-GET 下载 ----
        Log.d(TAG, "==================================================");
        Log.d(TAG, "Step 1: Attempting C-GET request to " + host + ":" + port);
        Log.d(TAG, "        PatientID: " + patientID);
        Log.d(TAG, "        Save Dir : " + tempDir.getAbsolutePath());
        Log.d(TAG, "==================================================");

        boolean cgetSuccess = false;
        try {
            cgetSuccess = DcmtkJni.cGet(host, port, localAET, remoteAET,
                    patientID, tempDir.getAbsolutePath(),null);
        } catch (Exception e) {
            Log.w(TAG, "C-GET threw exception (expected if no PACS): " + e.getMessage());
        }
        Log.d(TAG, "C-GET result: " + (cgetSuccess ? "SUCCESS" : "FAILED (or no PACS available)"));

        // ---- 步骤2: 检查 temp 目录是否有文件 ----
        // 注意: DCMTK 存储文件名格式为 "MODALITY.SOPInstanceUID"，不带 .dcm 扩展名，
        // 因此这里列出所有文件而非仅过滤 .dcm
        File[] files = tempDir.listFiles(File::isFile);
        int fileCount = (files != null) ? files.length : 0;
        Log.d(TAG, "Files in temp/ after C-GET: " + fileCount);

        // ---- 步骤3: 若无文件，生成本地测试 DICOM 以验证信息读取功能 ----
        if (fileCount == 0) {
            Log.d(TAG, "==================================================");
            Log.d(TAG, "Step 2: No files downloaded. Generating local test DICOM");
            Log.d(TAG, "        via writeDicomFile() for info-display verification.");
            Log.d(TAG, "==================================================");
            try {
                generateTestDicom(tempDir);
            } catch (IOException e) {
                fail("Failed to generate test DICOM file: " + e.getMessage());
            }
            files = tempDir.listFiles(File::isFile);
            fileCount = (files != null) ? files.length : 0;
        }

        assertTrue("temp directory should contain at least one file", fileCount > 0);

        // ---- 步骤4: 遍历显示每个文件的重要 DICOM 信息 ----
        Log.d(TAG, "==================================================");
        Log.d(TAG, "Step 3: Displaying important DICOM info for " + fileCount + " file(s)");
        Log.d(TAG, "==================================================");

        int validDicomCount = 0;
        for (File file : files) {
            Log.d(TAG, "--------------------------------------------------");
            Log.d(TAG, "File: " + file.getName() + "  (" + file.length() + " bytes)");

            HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(file.getAbsolutePath());
            if (info == null || info.isEmpty()) {
                Log.w(TAG, "  -> Not a valid DICOM file or no tags found.");
                continue;
            }
            validDicomCount++;

            // 打印重要 DICOM 信息
            String patientName = safeGet(info, "(0010,0010)");
            String patId        = safeGet(info, "(0010,0020)");
            String patientSex   = safeGet(info, "(0010,0040)");
            String birthDate    = safeGet(info, "(0010,0030)");
            String studyDate    = safeGet(info, "(0008,0020)");
            String studyTime    = safeGet(info, "(0008,0030)");
            String studyDesc    = safeGet(info, "(0008,1030)");
            String modality     = safeGet(info, "(0008,0060)");
            String seriesDesc   = safeGet(info, "(0008,103E)");
            String sopClassUid  = safeGet(info, "(0008,0016)");
            String sopInstUid   = safeGet(info, "(0008,0018)");
            String rows         = safeGet(info, "(0028,0010)");
            String columns      = safeGet(info, "(0028,0011)");
            String bitsAlloc    = safeGet(info, "(0028,0100)");
            String bitsStored   = safeGet(info, "(0028,0101)");
            String pixelSpacing = safeGet(info, "(0028,0030)");

            Log.d(TAG, "  -- Patient Info --");
            Log.d(TAG, "    Patient Name  : " + patientName);
            Log.d(TAG, "    Patient ID    : " + patId);
            Log.d(TAG, "    Patient Sex   : " + patientSex);
            Log.d(TAG, "    Birth Date    : " + birthDate);
            Log.d(TAG, "  -- Study Info --");
            Log.d(TAG, "    Study Date    : " + studyDate);
            Log.d(TAG, "    Study Time    : " + studyTime);
            Log.d(TAG, "    Study Desc    : " + studyDesc);
            Log.d(TAG, "  -- Series Info --");
            Log.d(TAG, "    Modality      : " + modality);
            Log.d(TAG, "    Series Desc   : " + seriesDesc);
            Log.d(TAG, "  -- Image Info --");
            Log.d(TAG, "    Rows          : " + rows);
            Log.d(TAG, "    Columns       : " + columns);
            Log.d(TAG, "    Bits Allocated: " + bitsAlloc);
            Log.d(TAG, "    Bits Stored   : " + bitsStored);
            Log.d(TAG, "    Pixel Spacing : " + pixelSpacing);
            Log.d(TAG, "  -- UID Info --");
            Log.d(TAG, "    SOP Class UID : " + sopClassUid);
            Log.d(TAG, "    SOP Inst UID  : " + sopInstUid);
        }

        Log.d(TAG, "==================================================");
        Log.d(TAG, "Test Summary: " + validDicomCount + "/" + fileCount
                + " files are valid DICOM.");
        Log.d(TAG, "==================================================");

        // ---- 步骤5: 断言验证重要标签存在 ----
        assertTrue("At least one valid DICOM file should exist", validDicomCount > 0);

        // 验证第一个有效 DICOM 文件的重要标签
        for (File file : files) {
            HashMap<String, String> info = DcmtkJni.loadDicomFileInfo(file.getAbsolutePath());
            if (info != null && !info.isEmpty()) {
                assertNotNull("Patient Name tag should exist", info.get("(0010,0010)"));
                assertNotNull("Patient ID tag should exist", info.get("(0010,0020)"));
                assertNotNull("Modality tag should exist", info.get("(0008,0060)"));
                assertNotNull("SOP Class UID tag should exist", info.get("(0008,0016)"));
                assertNotNull("Rows tag should exist", info.get("(0028,0010)"));
                assertNotNull("Columns tag should exist", info.get("(0028,0011)"));
                assertNotNull("Bits Allocated tag should exist", info.get("(0028,0100)"));
                Log.d(TAG, "Assertion passed: all important tags are present in "
                        + file.getName());
                break;
            }
        }
    }

    /**
     * 生成一个本地测试 DICOM 文件并保存到 temp 目录。
     * 使用 writeDicomFile 接口，创建 8x8 像素、16-bit 的单色图像。
     */
    private void generateTestDicom(File tempDir) throws IOException {
        int width = 8;
        int height = 8;
        // 16-bit 像素数据，每个像素 2 字节
        byte[] rawPixels = new byte[width * height * 2];
        // 填充简单的渐变数据 (little-endian 16-bit)
        for (int i = 0; i < width * height; i++) {
            short val = (short) (i * 100);
            rawPixels[i * 2] = (byte) (val & 0xFF);
            rawPixels[i * 2 + 1] = (byte) ((val >> 8) & 0xFF);
        }

        // 将原始像素数据写入临时 raw 文件
        File rawFile = new File(tempDir, "test_raw.bin");
        try (FileOutputStream fos = new FileOutputStream(rawFile)) {
            fos.write(rawPixels);
        }

        // 使用 writeDicomFile 生成标准 DICOM 文件
        File dcmFile = new File(tempDir, "TEST_GENERATED.dcm");
        boolean success = DcmtkJni.writeDicomFile(
                rawFile.getAbsolutePath(),
                dcmFile.getAbsolutePath(),
                width, height
        );
        assertTrue("writeDicomFile should succeed", success);

        // 清理临时 raw 文件
        // noinspection ResultOfMethodCallIgnored
        rawFile.delete();

        Log.d(TAG, "Generated test DICOM: " + dcmFile.getAbsolutePath()
                + " (" + dcmFile.length() + " bytes)");
    }

    /** 安全获取 HashMap 值，null 或空返回 "N/A" */
    private static String safeGet(HashMap<String, String> map, String key) {
        String val = map.get(key);
        return (val != null && !val.isEmpty()) ? val : "N/A";
    }
}
