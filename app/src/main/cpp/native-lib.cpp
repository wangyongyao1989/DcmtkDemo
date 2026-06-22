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
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Stubs for missing NDK symbols
extern "C" char* getlogin() { return (char*)"android"; }
extern "C" int getlogin_r(char* buf, size_t bufsize) {
    const char* user = "android";
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
        DcmDataDictionary& dict = dcmDataDict.wrlock();
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

    jclass mapClass = env->FindClass("java/util/HashMap");
    jmethodID mapInit = env->GetMethodID(mapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(mapClass, mapInit);
    jmethodID putMethod = env->GetMethodID(mapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    if (!path) return hashMap;

    DcmFileFormat fileformat;
    OFCondition status = fileformat.loadFile(path);
    env->ReleaseStringUTFChars(file_path, path);

    if (status.bad()) {
        LOGE("Failed to load DICOM: %s", status.text());
        return hashMap;
    }

    DcmDataset *dataset = fileformat.getDataset();
    dataset->loadAllDataIntoMemory();

    DcmStack stack;
    while (dataset->nextObject(stack, OFTrue).good()) {
        DcmObject *obj = stack.top();
        if (obj && obj->isLeaf()) {
            auto *element = dynamic_cast<DcmElement *>(obj);
            if (element) {
                DcmTag tag = element->getTag();
                char tagStr[32];
                snprintf(tagStr, sizeof(tagStr), "(%04X,%04X)", tag.getGroup(), tag.getElement());

                OFString valueStr;
                element->getOFStringArray(valueStr);

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

// JNI Registration
static const char* const kClassName = "com/example/dcmtkdemo/DcmtkJni";

static const JNINativeMethod kMethods[] = {
    {"stringFromJNI", "()Ljava/lang/String;", (void*)native_stringFromJNI},
    {"initDcmtk", "(Ljava/lang/String;)V", (void*)native_initDcmtk},
    {"loadDicomFileInfo", "(Ljava/lang/String;)Ljava/util/HashMap;", (void*)native_loadDicomFileInfo},
};

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass clazz = env->FindClass(kClassName);
    if (clazz == nullptr) return JNI_ERR;

    if (env->RegisterNatives(clazz, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0) {
        return JNI_ERR;
    }

    return JNI_VERSION_1_6;
}
