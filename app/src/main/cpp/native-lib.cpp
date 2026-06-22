#include <jni.h>
#include <string>
#include <cstdlib>
#include <android/bitmap.h>
#include <android/log.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

// Stub for getlogin and getlogin_r which are missing in some Android NDK/API levels
extern "C" char* getlogin() {
    return (char*)"android";
}

extern "C" int getlogin_r(char* buf, size_t bufsize) {
    const char* user = "android";
    size_t len = strlen(user);
    if (len >= bufsize) {
        return ERANGE;
    }
    strcpy(buf, user);
    return 0;
}

// DCMTK 核心头文件
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcdict.h" // 字典支持
#include "dcmtk/ofstd/ofcond.h"
#include "dcmtk/ofstd/ofstream.h"
#include "dcmtk/dcmjpeg/djencode.h"
#include "dcmtk/dcmdata/dcpxitem.h"
#include "dcmtk/dcmdata/dcostrma.h"
#include "dcmtk/dcmdata/dcspchrs.h"
#include "dcmtk/dcmdata/dcvrcs.h"
#include "dcmtk/dcmnet/scu.h"


#define ENABLE_LOGGING
#define TAG "dcmtk_android_jni"
#ifdef ENABLE_LOGGING
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#else
#define LOGD(...)
#define LOGE(...)
#define LOGI(...)
#define LOGW(...)
#define LOGV(...)
#endif

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_dcmtkdemo_DcmtkJni_stringFromJNI(JNIEnv *env, jobject thiz) {

    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}



extern "C"
JNIEXPORT void JNICALL
Java_com_example_dcmtkdemo_DcmtkJni_initDcmtk(JNIEnv *env, jclass clazz, jstring dict_path) {
    const char *path = env->GetStringUTFChars(dict_path, nullptr);

    // 加载 DICOM 字典
    DcmDataDictionary& dict = dcmDataDict.wrlock();
    dict.clear();
    dict.loadDictionary(path);
    dcmDataDict.wrunlock();

    env->ReleaseStringUTFChars(dict_path, path);
}

extern "C"
JNIEXPORT jobject JNICALL
Java_com_example_dcmtkdemo_DcmtkJni_loadDicomFileInfo(JNIEnv *env, jclass clazz, jstring file_path) {
    const char *path = env->GetStringUTFChars(file_path, nullptr);

    DcmFileFormat fileformat;
    OFCondition status = fileformat.loadFile(path);
    env->ReleaseStringUTFChars(file_path, path);

    // 准备 Java 的 HashMap
    jclass mapClass = env->FindClass("java/util/HashMap");
    jmethodID init = env->GetMethodID(mapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(mapClass, init);
    jmethodID putMethod = env->GetMethodID(mapClass, "put",
                                           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    if (!status.good()) {
        LOGE("Failed to load DICOM file: %s", status.text());
        return hashMap;
    }

    DcmDataset *dataset = fileformat.getDataset();
    dataset->loadAllDataIntoMemory();

    DcmStack stack;
    while (dataset->nextObject(stack, OFTrue).good()) {
        DcmObject *obj = stack.top();
        if (obj != nullptr && obj->isLeaf()) {
            auto *element = dynamic_cast<DcmElement *>(obj);
            if (element) {
                DcmTag tag = element->getTag();
                char tagStr[32];
                snprintf(tagStr, sizeof(tagStr), "(%04X,%04X)", tag.getGroup(), tag.getElement());

                OFString valueStr;
                element->getOFStringArray(valueStr);

                const char* tagName = tag.getTagName();
                // LOGD("Tag: %s %s : %s", tagStr, tagName ? tagName : "Unknown", valueStr.c_str());

                jstring key = env->NewStringUTF(tagStr);
                jstring val = env->NewStringUTF(valueStr.c_str());

                env->CallObjectMethod(hashMap, putMethod, key, val);

                env->DeleteLocalRef(key);
                env->DeleteLocalRef(val);
            }
        }
    }

    return hashMap;
}

