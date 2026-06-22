#include <jni.h>
#include <string>
#include <vector>
#include <android/log.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

// DCMTK Headers
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcdict.h"
#include "dcmtk/ofstd/ofcond.h"

#define TAG "DcmtkJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Stubs for missing NDK symbols
extern "C" char *getlogin() { return (char *) "android"; }
extern "C" int getlogin_r(char *buf, size_t bufsize) {
    const char *user = "android";
    if (strlen(user) >= bufsize) return ERANGE;
    strcpy(buf, user);
    return 0;
}

/**
 * Native implementation for DcmtkJni.stringFromJNI()
 */
static jstring native_stringFromJNI(JNIEnv *env, jobject thiz) {
    return env->NewStringUTF("Hello from DCMTK Native (Dynamic)");
}

/**
 * Native implementation for DcmtkJni.initDcmtk(String dictPath)
 */
static void native_initDcmtk(JNIEnv *env, jclass clazz, jstring dict_path) {
    const char *path = env->GetStringUTFChars(dict_path, nullptr);
    if (path) {
        DcmDataDictionary &dict = dcmDataDict.wrlock();
        dict.clear();
        dict.loadDictionary(path);
        dcmDataDict.wrunlock();
        env->ReleaseStringUTFChars(dict_path, path);
        LOGD("DCMTK Dictionary initialized from: %s", path);
    }
}

/**
 * Native implementation for DcmtkJni.loadDicomFileInfo(String filePath)
 */
static jobject native_loadDicomFileInfo(JNIEnv *env, jclass clazz, jstring file_path) {
    const char *path = env->GetStringUTFChars(file_path, nullptr);
    LOGD("native_loadDicomFileInfo: Entering with path: %s", path ? path : "NULL");

    jclass mapClass = env->FindClass("java/util/HashMap");
    jmethodID mapInit = env->GetMethodID(mapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(mapClass, mapInit);
    jmethodID putMethod = env->GetMethodID(mapClass, "put",
                                           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    if (!path) {
        LOGE("native_loadDicomFileInfo: Path is null");
        return hashMap;
    }

    DcmFileFormat fileformat;
    OFCondition status = fileformat.loadFile(path);
    LOGD("native_loadDicomFileInfo: loadFile status: %s", status.text());
    env->ReleaseStringUTFChars(file_path, path);

    if (status.bad()) {
        LOGE("native_loadDicomFileInfo: Failed to load DICOM: %s", status.text());
        return hashMap;
    }

    DcmDataset *dataset = fileformat.getDataset();
    LOGD("native_loadDicomFileInfo: Dataset retrieved, loading all data into memory...");
    dataset->loadAllDataIntoMemory();

    int elementCount = 0;
    DcmStack stack;
    while (dataset->nextObject(stack, OFTrue).good()) {
        DcmObject *obj = stack.top();
        if (obj && obj->isLeaf()) {
            auto *element = dynamic_cast<DcmElement *>(obj);
            if (element) {
                DcmTag tag = element->getTag();
                char tagStr[32];
                snprintf(tagStr, sizeof(tagStr), "(%04X,%04X)",
                         tag.getGroup(), tag.getElement());

                OFString valueStr;
                element->getOFStringArray(valueStr);

                jstring key = env->NewStringUTF(tagStr);
                jstring val = env->NewStringUTF(valueStr.c_str());
                env->CallObjectMethod(hashMap, putMethod, key, val);
                env->DeleteLocalRef(key);
                env->DeleteLocalRef(val);
                elementCount++;
            }
        }
    }

    LOGD("native_loadDicomFileInfo: Finished processing, found %d leaf elements", elementCount);
    return hashMap;
}

/**
 * Native implementation for DcmtkJni.writeDicomFile(String rawPath, String dcmPath, int width, int height)
 */
static jboolean native_writeDicomFile(JNIEnv *env, jclass clazz, jstring raw_path,
                                      jstring dcm_path, jint width, jint height) {
    const char *src_path = env->GetStringUTFChars(raw_path, nullptr);
    const char *dest_path = env->GetStringUTFChars(dcm_path, nullptr);

    LOGD("native_writeDicomFile: src=%s, dest=%s, w=%d, h=%d", src_path, dest_path, width, height);

    jboolean success = JNI_FALSE;

    FILE *f = fopen(src_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        LOGD("native_writeDicomFile: Opened raw file, size=%ld bytes", size);

        unsigned char *pixelData = new unsigned char[size];
        size_t readSize = fread(pixelData, 1, size, f);
        fclose(f);
        LOGD("native_writeDicomFile: Read %zu bytes from raw file", readSize);

        DcmFileFormat fileformat;
        DcmDataset *dataset = fileformat.getDataset();

        LOGD("native_writeDicomFile: Setting DICOM metadata tags...");
        dataset->putAndInsertString(DCM_SOPClassUID, UID_SecondaryCaptureImageStorage);
        char uid[100];
        dcmGenerateUniqueIdentifier(uid, SITE_INSTANCE_UID_ROOT);
        dataset->putAndInsertString(DCM_SOPInstanceUID, uid);
        dataset->putAndInsertString(DCM_PatientName, "Demo^Dcmtk");
        dataset->putAndInsertString(DCM_PatientID, "12345678");
        dataset->putAndInsertString(DCM_Modality, "OT");
        dataset->putAndInsertString(DCM_StudyDate, "20240101");

        dataset->putAndInsertUint16(DCM_SamplesPerPixel, 1);
        dataset->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
        dataset->putAndInsertUint16(DCM_Rows, (Uint16) height);
        dataset->putAndInsertUint16(DCM_Columns, (Uint16) width);
        dataset->putAndInsertUint16(DCM_BitsAllocated, 16);
        dataset->putAndInsertUint16(DCM_BitsStored, 16);
        dataset->putAndInsertUint16(DCM_HighBit, 15);
        dataset->putAndInsertUint16(DCM_PixelRepresentation, 0);

        // --- Calculate Optimal Window Center and Width (Min-Max algorithm) ---
        double minVal = 65535.0;
        double maxVal = 0.0;
        Uint16 *ptr16 = (Uint16 *) pixelData;
        size_t numPixels = size / 2;

        if (numPixels > 0) {
            for (size_t i = 0; i < numPixels; ++i) {
                Uint16 val = ptr16[i];
                if (val < minVal) minVal = val;
                if (val > maxVal) maxVal = val;
            }

            double windowWidth = maxVal - minVal;
            double windowCenter = minVal + (windowWidth / 2.0);

            // Window Width must be at least 1.0 according to DICOM standard
            if (windowWidth < 1.0) windowWidth = 1.0;

            LOGD("native_writeDicomFile: Calculated Min=%f, Max=%f -> WC=%f, WW=%f",
                 minVal, maxVal, windowCenter, windowWidth);

            char wcStr[32], wwStr[32];
            snprintf(wcStr, sizeof(wcStr), "%.2f", windowCenter);
            snprintf(wwStr, sizeof(wwStr), "%.2f", windowWidth);

            dataset->putAndInsertString(DCM_WindowCenter, wcStr);
            dataset->putAndInsertString(DCM_WindowWidth, wwStr);
        }
        // ---------------------------------------------------------------------

        LOGD("native_writeDicomFile: Inserting pixel data...");
        if (size % 2 == 0) {
            dataset->putAndInsertUint16Array(DCM_PixelData, (Uint16 *) pixelData,
                                             (Uint32) (size / 2));
        } else {
            LOGW("native_writeDicomFile: Size is odd (%ld), inserting as Uint8", size);
            dataset->putAndInsertUint8Array(DCM_PixelData, pixelData, (Uint32) size);
        }

        LOGD("native_writeDicomFile: Saving file to: %s", dest_path);
        OFCondition status = fileformat.saveFile(dest_path, EXS_LittleEndianExplicit);
        if (status.good()) {
            success = JNI_TRUE;
            LOGD("native_writeDicomFile: Successfully wrote DICOM");
        } else {
            LOGE("native_writeDicomFile: Failed to save DICOM: %s", status.text());
        }

        delete[] pixelData;
    } else {
        LOGE("native_writeDicomFile: Failed to open source raw file: %s, errno: %d (%s)",
             src_path, errno, strerror(errno));
    }

    env->ReleaseStringUTFChars(raw_path, src_path);
    env->ReleaseStringUTFChars(dcm_path, dest_path);
    return success;
}

// JNI Registration
static const char *const kClassName = "com/example/dcmtkdemo/DcmtkJni";

static const JNINativeMethod kMethods[] = {
        {"stringFromJNI",     "()Ljava/lang/String;",
                (void *) native_stringFromJNI},
        {"initDcmtk",         "(Ljava/lang/String;)V",
                (void *) native_initDcmtk},
        {"loadDicomFileInfo", "(Ljava/lang/String;)Ljava/util/HashMap;",
                (void *) native_loadDicomFileInfo},
        {"writeDicomFile",    "(Ljava/lang/String;Ljava/lang/String;II)Z",
                (void *) native_writeDicomFile},

};

extern "C" jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass clazz = env->FindClass(kClassName);
    if (clazz == nullptr) return JNI_ERR;

    if (env->RegisterNatives(clazz, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0) {
        return JNI_ERR;
    }

    return JNI_VERSION_1_6;
}
