#include <jni.h>
#include <string>
#include <cstdlib>
#include <android/bitmap.h>
// DCMTK 核心头文件
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmimgle/dcmimage.h>
#include <dcmtk/ofstd/ofcond.h>

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_dcmtkdemo_DcmtkJni_stringFromJNI(JNIEnv *env, jobject thiz) {

    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}



extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_dcmtkdemo_DcmtkJni_initDcmtk(JNIEnv *env, jclass clazz, jstring dict_path) {
    const char *dictPath = env->GetStringUTFChars(dict_path, nullptr);
    if (dictPath == nullptr) {
        return JNI_FALSE;
    }

    // 设置 DICOM 字典环境变量，DCMTK 运行时必须加载
    setenv("DCMDICTPATH", dictPath, 1);
    // 设置临时文件目录为 Android 应用私有缓存
    // 实际使用时建议传入应用缓存目录，这里先使用系统默认临时目录
    setenv("TMPDIR", "/data/local/tmp", 1);

    env->ReleaseStringUTFChars(dict_path, dictPath);
    return JNI_TRUE;

}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_dcmtkdemo_DcmtkJni_getDicomInfo(JNIEnv *env, jclass clazz, jstring file_path) {

    const char *filePath = env->GetStringUTFChars(file_path, nullptr);
    if (filePath == nullptr) {
        return env->NewStringUTF("文件路径为空");
    }

    // 打开 DICOM 文件
    DcmFileFormat fileFormat;
    OFCondition status = fileFormat.loadFile(filePath);
    env->ReleaseStringUTFChars(file_path, filePath);

    if (!status.good()) {
        std::string err = "打开文件失败: ";
        err += status.text();
        return env->NewStringUTF(err.c_str());
    }

    DcmDataset *dataset = fileFormat.getDataset();
    OFString value;
    std::string result;

    // 读取患者信息
    dataset->findAndGetOFString(DCM_PatientName, value);
    result += "患者姓名: " + std::string(value.c_str()) + "\n";

    dataset->findAndGetOFString(DCM_PatientID, value);
    result += "患者ID: " + std::string(value.c_str()) + "\n";

    dataset->findAndGetOFString(DCM_StudyDate, value);
    result += "检查日期: " + std::string(value.c_str()) + "\n";

    dataset->findAndGetOFString(DCM_Modality, value);
    result += "检查模态: " + std::string(value.c_str()) + "\n";

    // 读取图像信息
    Uint16 rows = 0, cols = 0;
    dataset->findAndGetUint16(DCM_Rows, rows);
    dataset->findAndGetUint16(DCM_Columns, cols);
    result += "图像尺寸: " + std::to_string(cols) + " x " + std::to_string(rows) + "\n";

    Uint16 bitsAllocated = 0;
    dataset->findAndGetUint16(DCM_BitsAllocated, bitsAllocated);
    result += "位深度: " + std::to_string(bitsAllocated) + " bit\n";

    return env->NewStringUTF(result.c_str());
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_dcmtkdemo_DcmtkJni_renderDicomToBitmap(JNIEnv *env, jclass clazz,
                                                        jstring file_path, jobject bitmap) {

    const char *filePath = env->GetStringUTFChars(file_path, nullptr);
    if (filePath == nullptr) {
        return JNI_FALSE;
    }

    // 创建 DICOM 图像对象，自动解析像素数据
    DicomImage *dicomImage = new DicomImage(filePath);
    env->ReleaseStringUTFChars(file_path, filePath);

    if (dicomImage == nullptr || dicomImage->getStatus() != EIS_Normal) {
        delete dicomImage;
        return JNI_FALSE;
    }

    // 获取图像宽高
    const int width = dicomImage->getWidth();
    const int height = dicomImage->getHeight();

    // 锁定 Bitmap，获取像素缓冲区
    AndroidBitmapInfo bitmapInfo;
    void *bitmapPixels;
    int ret = AndroidBitmap_getInfo(env, bitmap, &bitmapInfo);
    if (ret != 0 || bitmapInfo.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        delete dicomImage;
        return JNI_FALSE;
    }

    ret = AndroidBitmap_lockPixels(env, bitmap, &bitmapPixels);
    if (ret != 0) {
        delete dicomImage;
        return JNI_FALSE;
    }

    // 设置默认窗宽窗位：使用全范围灰度
    // 如需使用文件内置窗宽窗位，可替换为 dicomImage->setWindow(0) 调用第 0 组窗宽窗位
    dicomImage->setMinMaxWindow();

    // 获取 8 位灰度输出数据（0-255）
    const Uint8 *pixelData = (const Uint8 *) dicomImage->getOutputData(8);
    if (pixelData == nullptr) {
        AndroidBitmap_unlockPixels(env, bitmap);
        delete dicomImage;
        return JNI_FALSE;
    }

    // 将灰度数据填充为 ARGB_8888 格式
    auto *dst = (uint32_t *) bitmapPixels;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            Uint8 gray = pixelData[y * width + x];
            // 灰度图 R=G=B，Alpha 设为 255 不透明
            dst[y * width + x] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
        }
    }

    // 释放资源
    AndroidBitmap_unlockPixels(env, bitmap);
    delete dicomImage;

    return JNI_TRUE;
}