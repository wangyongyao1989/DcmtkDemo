#include <jni.h>
#include <string.h>
#include <errno.h>

#include <android/bitmap.h>
#include <android/log.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

// DCMTK: only dictionary setup runs in this bridge (initDcmtk); all other
// DCMTK usage lives in the decoupled business classes (PacsClient/DicomFileIO).
#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcdict.h"

#include "DicomFileIO.h"
#include "JniString.h"
#include "PacsClient.h"

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

// =============================================================================
// JNI bridge: each native_* function only marshals JNI types <-> C++ types and
// delegates to PacsClient / DicomFileIO. All JNIEnv usage stays here so the
// business classes remain JNI-free and independently readable.
// =============================================================================

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
        LOGD("DCMTK Dictionary initialized from: %s", path);
        env->ReleaseStringUTFChars(dict_path, path);
    }
}

/**
 * Native implementation for DcmtkJni.loadDicomFileInfo(String filePath)
 */
static jobject native_loadDicomFileInfo(JNIEnv *env, jclass clazz,
                                        jstring file_path) {
    jclass mapClass = env->FindClass("java/util/HashMap");
    jmethodID mapInit = env->GetMethodID(mapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(mapClass, mapInit);
    jmethodID putMethod = env->GetMethodID(mapClass, "put",
                                           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    JniString path(env, file_path);
    if (!path.c_str()) {
        LOGE("native_loadDicomFileInfo: Path is null");
        return hashMap;
    }

    std::map<std::string, std::string> info = DicomFileIO::loadFileInfo(path.c_str());
    for (const auto &kv: info) {
        jstring key = env->NewStringUTF(kv.first.c_str());
        jstring val = env->NewStringUTF(kv.second.c_str());
        env->CallObjectMethod(hashMap, putMethod, key, val);
        env->DeleteLocalRef(key);
        env->DeleteLocalRef(val);
    }
    return hashMap;
}

/**
 * Native implementation for DcmtkJni.writeDicomFile(String rawPath, String dcmPath, int width, int height)
 */
static jboolean native_writeDicomFile(JNIEnv *env, jclass clazz, jstring raw_path,
                                      jstring dcm_path, jint width, jint height) {
    JniString raw(env, raw_path);
    JniString dcm(env, dcm_path);
    bool ok = DicomFileIO::writeDicomFile(raw.c_str() ? raw.c_str() : "",
                                          dcm.c_str() ? dcm.c_str() : "",
                                          width, height);
    return ok ? JNI_TRUE : JNI_FALSE;
}

static jboolean native_connectPACS(JNIEnv *env, jclass clazz, jstring host, jint port,
                                   jstring local_aet, jstring remote_aet) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    bool ok = PacsClient::connectPACS(c_host.c_str() ? c_host.c_str() : "", port,
                                      c_local.c_str() ? c_local.c_str() : "",
                                      c_remote.c_str() ? c_remote.c_str() : "");
    return ok ? JNI_TRUE : JNI_FALSE;
}

static jboolean native_cEcho(JNIEnv *env, jclass clazz, jstring host, jint port,
                             jstring local_aet, jstring remote_aet) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    bool ok = PacsClient::cEcho(c_host.c_str() ? c_host.c_str() : "", port,
                                c_local.c_str() ? c_local.c_str() : "",
                                c_remote.c_str() ? c_remote.c_str() : "");
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Adapts a Java ProgressCallback into the std::function expected by PacsClient.
// `env` and `callback` are valid for the duration of the synchronous native
// call (same thread), so no global reference is needed -- matches the original
// ProgressSCU semantics.
static PacsClient::ProgressCallback makeProgressCallback(JNIEnv *env, jobject callback) {
    if (callback == nullptr) return nullptr;
    return [env, callback](unsigned long sent, unsigned long total) {
        jclass cls = env->GetObjectClass(callback);
        if (cls) {
            jmethodID mid = env->GetMethodID(cls, "onProgress", "(JJ)V");
            if (mid) {
                env->CallVoidMethod(callback, mid, (jlong) sent, (jlong) total);
            }
            env->DeleteLocalRef(cls);
        }
    };
}

static jboolean native_cStore(JNIEnv *env, jclass clazz, jstring host, jint port,
                              jstring local_aet, jstring remote_aet, jstring dcm_path,
                              jobject callback) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    JniString c_dcm(env, dcm_path);

    bool ok = PacsClient::cStore(c_host.c_str() ? c_host.c_str() : "", port,
                                 c_local.c_str() ? c_local.c_str() : "",
                                 c_remote.c_str() ? c_remote.c_str() : "",
                                 c_dcm.c_str() ? c_dcm.c_str() : "",
                                 makeProgressCallback(env, callback));
    return ok ? JNI_TRUE : JNI_FALSE;
}

static jint native_cStoreMulti(JNIEnv *env, jclass clazz, jstring host, jint port,
                               jstring local_aet, jstring remote_aet, jobjectArray dcm_paths,
                               jobject callback) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);

    int numPaths = env->GetArrayLength(dcm_paths);
    std::vector<std::string> paths;
    for (int i = 0; i < numPaths; ++i) {
        jstring pathObj = (jstring) env->GetObjectArrayElement(dcm_paths, i);
        if (pathObj) {
            JniString path(env, pathObj);
            if (path.c_str()) paths.push_back(path.c_str());
        }
        env->DeleteLocalRef(pathObj);
    }

    auto multiCallback = [env, callback](int index,
                                         unsigned long sent, unsigned long total,
                                         bool finished, bool success) -> bool {
        if (callback) {
            jclass cls = env->GetObjectClass(callback);
            if (cls) {
                if (finished) {
                    jmethodID midStatus = env->GetMethodID(cls, "onItemStatus", "(IZ)V");
                    if (midStatus) {
                        env->CallVoidMethod(callback, midStatus, (jint) index, (jboolean) success);
                    }
                }

                jmethodID mid = env->GetMethodID(cls, "onProgress", "(IJJ)Z");
                if (mid) {
                    jboolean result = env->CallBooleanMethod(callback, mid, (jint) index,
                                                             (jlong) sent, (jlong) total);
                    env->DeleteLocalRef(cls);
                    return (result == JNI_TRUE);
                }
                env->DeleteLocalRef(cls);
            }
        }
        return true;
    };

    return (jint) PacsClient::cStoreMulti(c_host.c_str() ? c_host.c_str() : "", port,
                                          c_local.c_str() ? c_local.c_str() : "",
                                          c_remote.c_str() ? c_remote.c_str() : "",
                                          paths, multiCallback);
}

static jobjectArray native_cFind(JNIEnv *env, jclass clazz, jstring host, jint port,
                                 jstring local_aet, jstring remote_aet, jstring patient_name) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    JniString c_pat(env, patient_name);

    std::vector<std::string> results = PacsClient::cFind(
            c_host.c_str() ? c_host.c_str() : "", port,
            c_local.c_str() ? c_local.c_str() : "",
            c_remote.c_str() ? c_remote.c_str() : "",
            c_pat.c_str() ? c_pat.c_str() : "");

    jobjectArray ret = (jobjectArray) env->NewObjectArray(results.size(),
                                                          env->FindClass("java/lang/String"),
                                                          env->NewStringUTF(""));
    for (size_t i = 0; i < results.size(); ++i) {
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(results[i].c_str()));
    }
    return ret;
}

static jobjectArray native_cFindByAccession(JNIEnv *env, jclass clazz, jstring host, jint port,
                                            jstring local_aet, jstring remote_aet,
                                            jstring accession_number) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    JniString c_acc(env, accession_number);

    std::vector<std::string> results = PacsClient::cFindByAccession(
            c_host.c_str() ? c_host.c_str() : "", port,
            c_local.c_str() ? c_local.c_str() : "",
            c_remote.c_str() ? c_remote.c_str() : "",
            c_acc.c_str() ? c_acc.c_str() : "");

    jobjectArray ret = (jobjectArray) env->NewObjectArray(results.size(),
                                                          env->FindClass("java/lang/String"),
                                                          env->NewStringUTF(""));
    for (size_t i = 0; i < results.size(); ++i) {
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(results[i].c_str()));
    }
    return ret;
}

static jobjectArray native_cFindMWL(JNIEnv *env, jclass clazz, jstring host, jint port,
                                    jstring local_aet, jstring remote_aet, jstring modality) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    JniString c_mod(env, modality);

    std::vector<DcmDataset*> results = PacsClient::cFindMWL(
            c_host.c_str() ? c_host.c_str() : "", port,
            c_local.c_str() ? c_local.c_str() : "",
            c_remote.c_str() ? c_remote.c_str() : "",
            c_mod.c_str() ? c_mod.c_str() : "");

    jclass mapClass = env->FindClass("java/util/HashMap");
    jmethodID mapInit = env->GetMethodID(mapClass, "<init>", "()V");
    jmethodID putMethod = env->GetMethodID(mapClass, "put",
                                           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

    jobjectArray ret = (jobjectArray) env->NewObjectArray(results.size(), mapClass, nullptr);

    for (size_t i = 0; i < results.size(); ++i) {
        DcmDataset *ds = results[i];
        jobject hashMap = env->NewObject(mapClass, mapInit);
        if (ds) {
            DcmStack stack;
            while (ds->nextObject(stack, OFTrue).good()) {
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
                    }
                }
            }
            delete ds; // Important: delete cloned dataset
        }
        env->SetObjectArrayElement(ret, i, hashMap);
        env->DeleteLocalRef(hashMap);
    }
    return ret;
}

static jobjectArray native_cFindMWLByTemplate(JNIEnv *env, jclass clazz, jstring host, jint port,
                                              jstring local_aet, jstring remote_aet,
                                              jstring template_path, jstring output_dir) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    JniString c_tpl(env, template_path);
    JniString c_out(env, output_dir);

    std::vector<std::string> results = PacsClient::cFindMWLByTemplate(
            c_host.c_str() ? c_host.c_str() : "", port,
            c_local.c_str() ? c_local.c_str() : "",
            c_remote.c_str() ? c_remote.c_str() : "",
            c_tpl.c_str() ? c_tpl.c_str() : "",
            c_out.c_str() ? c_out.c_str() : "");

    jobjectArray ret = (jobjectArray) env->NewObjectArray(results.size(),
                                                          env->FindClass("java/lang/String"),
                                                          env->NewStringUTF(""));
    for (size_t i = 0; i < results.size(); ++i) {
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(results[i].c_str()));
    }
    return ret;
}

static jboolean native_cMove(JNIEnv *env, jclass clazz, jstring host, jint port,
                             jstring local_aet, jstring remote_aet, jstring patient_id,
                             jstring dest_aet) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    JniString c_pat(env, patient_id);
    JniString c_dest(env, dest_aet);
    bool ok = PacsClient::cMove(c_host.c_str() ? c_host.c_str() : "", port,
                                c_local.c_str() ? c_local.c_str() : "",
                                c_remote.c_str() ? c_remote.c_str() : "",
                                c_pat.c_str() ? c_pat.c_str() : "",
                                c_dest.c_str() ? c_dest.c_str() : "");
    return ok ? JNI_TRUE : JNI_FALSE;
}

static jboolean native_cGet(JNIEnv *env, jclass clazz, jstring host, jint port,
                            jstring local_aet, jstring remote_aet, jstring patient_id,
                            jstring save_dir, jobject callback) {
    JniString c_host(env, host);
    JniString c_local(env, local_aet);
    JniString c_remote(env, remote_aet);
    JniString c_pat(env, patient_id);
    JniString c_save(env, save_dir);

    bool ok = PacsClient::cGet(c_host.c_str() ? c_host.c_str() : "", port,
                               c_local.c_str() ? c_local.c_str() : "",
                               c_remote.c_str() ? c_remote.c_str() : "",
                               c_pat.c_str() ? c_pat.c_str() : "",
                               c_save.c_str() ? c_save.c_str() : "",
                               makeProgressCallback(env, callback));
    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * Native implementation for DcmtkJni.dcmToJpg(String dir)
 */
static jint native_dcmToJpg(JNIEnv *env, jclass clazz, jstring dir_path) {
    JniString c_dir(env, dir_path);
    return DicomFileIO::dcmToJpg(c_dir.c_str() ? c_dir.c_str() : "");
}

static void native_cancelOperation(JNIEnv *env, jclass clazz) {
    PacsClient::cancelOperation();
}

// =============================================================================
// 对应 dcm4che3 版 DicomFileUtils.kt 的新增 JNI 方法
// =============================================================================

// HashMap 构造辅助：把 std::map<string,string> 填入新建的 java.util.HashMap
static jobject buildStringMap(JNIEnv *env, const std::map<std::string, std::string> &m) {
    jclass mapClass = env->FindClass("java/util/HashMap");
    jmethodID mapInit = env->GetMethodID(mapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(mapClass, mapInit);
    jmethodID putMethod = env->GetMethodID(mapClass, "put",
                                           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    for (const auto &kv: m) {
        jstring key = env->NewStringUTF(kv.first.c_str());
        jstring val = env->NewStringUTF(kv.second.c_str());
        env->CallObjectMethod(hashMap, putMethod, key, val);
        env->DeleteLocalRef(key);
        env->DeleteLocalRef(val);
    }
    return hashMap;
}

static jobject native_loadDicomFileInfoEx(JNIEnv *env, jclass clazz, jstring file_path) {
    JniString path(env, file_path);
    auto info = DicomFileIO::loadFileInfoNamed(path.c_str() ? path.c_str() : "");
    return buildStringMap(env, info);
}

static jobject native_readDicomWindowSettingsNative(JNIEnv *env, jclass clazz, jstring file_path) {
    JniString path(env, file_path);
    auto info = DicomFileIO::readWindowSettings(path.c_str() ? path.c_str() : "");
    return buildStringMap(env, info);
}

// 用 RGBA8888 数据创建 android.graphics.Bitmap
static jobject createRgbaBitmap(JNIEnv *env, int width, int height,
                                const std::vector<uint8_t> &rgba) {
    jclass bmpCls = env->FindClass("android/graphics/Bitmap");
    jmethodID createBmp = env->GetStaticMethodID(
            bmpCls, "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jclass cfgCls = env->FindClass("android/graphics/Bitmap$Config");
    jfieldID argbField = env->GetStaticFieldID(cfgCls, "ARGB_8888",
                                               "Landroid/graphics/Bitmap$Config;");
    jobject config = env->GetStaticObjectField(cfgCls, argbField);
    jobject bitmap = env->CallStaticObjectMethod(bmpCls, createBmp, width, height, config);
    env->DeleteLocalRef(config);
    if (!bitmap) return nullptr;

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) {
        LOGE("createRgbaBitmap: getInfo failed");
        return bitmap;
    }
    void *pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) {
        LOGE("createRgbaBitmap: lockPixels failed");
        return bitmap;
    }
    memcpy(pixels, rgba.data(), rgba.size());
    AndroidBitmap_unlockPixels(env, bitmap);
    return bitmap;
}

static jobject native_dicomFile2Bitmap(JNIEnv *env, jclass clazz, jstring file_path) {
    JniString path(env, file_path);
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!DicomFileIO::dicomFileToBitmapRgba(path.c_str() ? path.c_str() : "",
                                            false, 0.0, 0.0, rgba, w, h)) {
        return nullptr;
    }
    return createRgbaBitmap(env, w, h, rgba);
}

static jobject native_dicomFile2BitmapWW(JNIEnv *env, jclass clazz, jstring file_path,
                                         jdouble ww, jdouble wc) {
    JniString path(env, file_path);
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!DicomFileIO::dicomFileToBitmapRgba(path.c_str() ? path.c_str() : "",
                                            true, ww, wc, rgba, w, h)) {
        return nullptr;
    }
    return createRgbaBitmap(env, w, h, rgba);
}

// 读取 ScanRecord 各字段
struct ScanRecordFields {
    jint examineNo;
    std::string patientName;
    std::string patientAge;
    std::string patientSex;
    std::string toothPosition;
};

static bool readScanRecord(JNIEnv *env, jobject record, ScanRecordFields &out) {
    if (!record) return false;
    jclass cls = env->GetObjectClass(record);
    jfieldID fExamine = env->GetFieldID(cls, "examineNo", "I");
    jfieldID fName = env->GetFieldID(cls, "patientName", "Ljava/lang/String;");
    jfieldID fAge = env->GetFieldID(cls, "patientAge", "Ljava/lang/String;");
    jfieldID fSex = env->GetFieldID(cls, "patientSex", "Ljava/lang/String;");
    jfieldID fTooth = env->GetFieldID(cls, "toothPosition", "Ljava/lang/String;");
    if (!fExamine || !fName || !fAge || !fSex || !fTooth) {
        env->DeleteLocalRef(cls);
        LOGE("readScanRecord: field id missing");
        return false;
    }
    out.examineNo = env->GetIntField(record, fExamine);

    auto readStr = [&](jfieldID fid) -> std::string {
        jstring s = (jstring) env->GetObjectField(record, fid);
        if (!s) return std::string();
        JniString js(env, s);
        std::string v = js.c_str() ? js.c_str() : "";
        env->DeleteLocalRef(s);
        return v;
    };
    out.patientName = readStr(fName);
    out.patientAge = readStr(fAge);
    out.patientSex = readStr(fSex);
    out.toothPosition = readStr(fTooth);
    env->DeleteLocalRef(cls);
    return true;
}

static jobject native_writeDcmFile(JNIEnv *env, jclass clazz, jobject record,
                                   jstring raw_path, jstring dcm_path,
                                   jint width, jint height) {
    ScanRecordFields rec;
    if (!readScanRecord(env, record, rec)) {
        return nullptr;
    }
    DicomFileIO::ScanRecordInfo info;
    info.examineNo = rec.examineNo;
    info.patientName = rec.patientName;
    info.patientAge = rec.patientAge;
    info.patientSex = rec.patientSex;
    info.toothPosition = rec.toothPosition;

    JniString raw(env, raw_path);
    JniString dcm(env, dcm_path);
    DicomFileIO::PixelDataInfo px;
    if (!DicomFileIO::writeDcmFileFull(raw.c_str() ? raw.c_str() : "",
                                       dcm.c_str() ? dcm.c_str() : "",
                                       width, height, info, px)) {
        return nullptr;
    }

    // 构造 PixelData(rows, columns, data:ByteArray, win_width, win_center,
    //                exposure_leve, largestImagePixelValue)
    jclass pxClass = env->FindClass("com/example/dcmtk/model/PixelData");
    if (!pxClass) {
        LOGE("native_writeDcmFile: PixelData class not found");
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(pxClass, "<init>",
                                      "(II[BDDII)V");
    if (!ctor) {
        LOGE("native_writeDcmFile: PixelData ctor not found");
        return nullptr;
    }
    jbyteArray dataArr = env->NewByteArray((jsize) px.data.size());
    if (!dataArr) {
        LOGE("native_writeDcmFile: NewByteArray failed");
        return nullptr;
    }
    env->SetByteArrayRegion(dataArr, 0, (jsize) px.data.size(),
                            (const jbyte *) px.data.data());
    jobject result = env->NewObject(pxClass, ctor,
                                    (jint) px.rows, (jint) px.columns, dataArr,
                                    (jdouble) px.win_width, (jdouble) px.win_center,
                                    (jint) px.exposure_leve, (jint) px.largestImagePixelValue);
    env->DeleteLocalRef(dataArr);
    return result;
}

// JNI Registration
static const char *const kClassName = "com/example/dcmtk/jni/DcmtkJni";

static const JNINativeMethod kMethods[] = {
        {"stringFromJNI",
                "()Ljava/lang/String;",
                (void *) native_stringFromJNI},
        {"initDcmtk",
                "(Ljava/lang/String;)V",
                (void *) native_initDcmtk},
        {"loadDicomFileInfo",
                "(Ljava/lang/String;)Ljava/util/HashMap;",
                (void *) native_loadDicomFileInfo},
        {"writeDicomFile",
                "(Ljava/lang/String;Ljava/lang/String;II)Z",
                (void *) native_writeDicomFile},
        {"connectPACS",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)Z",
                (void *) native_connectPACS},
        {"cEcho",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)Z",
                (void *) native_cEcho},
        {"cStore",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Lcom/example/dcmtk/callback/ProgressCallback;)Z",
                (void *) native_cStore},
        {"cStoreMulti",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;[Ljava/lang/String;Lcom/example/dcmtk/callback/MultiProgressCallback;)I",
                (void *) native_cStoreMulti},
        {"cFind",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;",
                (void *) native_cFind},
        {"cFindByAccession",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;",
                (void *) native_cFindByAccession},
        {"cFindMWL",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/util/HashMap;",
                (void *) native_cFindMWL},
        {"cFindMWLByTemplate",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;",
                (void *) native_cFindMWLByTemplate},
        {"cMove",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
                (void *) native_cMove},
        {"cGet",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lcom/example/dcmtk/callback/ProgressCallback;)Z",
                (void *) native_cGet},
        {"dcmToJpg",
                "(Ljava/lang/String;)I",
                (void *) native_dcmToJpg},
        {"cancelOperation",
                "()V",
                (void *) native_cancelOperation},
        {"loadDicomFileInfoEx",
                "(Ljava/lang/String;)Ljava/util/HashMap;",
                (void *) native_loadDicomFileInfoEx},
        {"readDicomWindowSettingsNative",
                "(Ljava/lang/String;)Ljava/util/HashMap;",
                (void *) native_readDicomWindowSettingsNative},
        {"dicomFile2Bitmap",
                "(Ljava/lang/String;)Landroid/graphics/Bitmap;",
                (void *) native_dicomFile2Bitmap},
        {"dicomFile2BitmapWW",
                "(Ljava/lang/String;DD)Landroid/graphics/Bitmap;",
                (void *) native_dicomFile2BitmapWW},
        {"writeDcmFile",
                "(Lcom/example/dcmtk/model/ScanRecord;Ljava/lang/String;Ljava/lang/String;II)Lcom/example/dcmtk/model/PixelData;",
                (void *) native_writeDcmFile},

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
