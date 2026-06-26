#include <jni.h>
#include <string>
#include <vector>
#include <android/log.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <memory>
#include <dirent.h>
#include <sys/stat.h>

// DCMTK Headers
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcdict.h"
#include "dcmtk/ofstd/ofcond.h"
#include "dcmtk/dcmnet/assoc.h"
#include "dcmtk/dcmnet/dimse.h"
#include "dcmtk/dcmnet/scu.h"
#include "dcmtk/dcmimgle/dcmimage.h"
#include "dcmtk/dcmjpeg/dipijpeg.h"
#include "dcmtk/dcmjpeg/djdecode.h"

#define TAG "DcmtkJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// RAII helper for JNI strings
class JniString {
public:
    JniString(JNIEnv *env, jstring str) : env_(env), jstr_(str), c_str_(nullptr) {
        if (jstr_) c_str_ = env_->GetStringUTFChars(jstr_, nullptr);
    }

    ~JniString() {
        if (c_str_) env_->ReleaseStringUTFChars(jstr_, c_str_);
    }

    const char *c_str() const { return c_str_; }

    operator const char *() const { return c_str_; }

private:
    JNIEnv *env_;
    jstring jstr_;
    const char *c_str_;
};

// Common Transfer Syntaxes helper
static void addCommonTransferSyntaxes(OFList<OFString> &ts) {
    ts.push_back(UID_LittleEndianExplicitTransferSyntax);
    ts.push_back(UID_BigEndianExplicitTransferSyntax);
    ts.push_back(UID_LittleEndianImplicitTransferSyntax);
}

// DcmSCU subclass that reports send/receive progress back to Java via JNI.
// The JNIEnv* and jobject are valid for the duration of the synchronous
// sendSTORERequest()/sendCGETRequest() call (same thread), so no global ref is needed.
class ProgressSCU : public DcmSCU {
public:
    ProgressSCU()
        : DcmSCU()
        , m_env(nullptr)
        , m_callback(nullptr)
        , m_totalBytes(0)
        , m_lastRecvBytes(0)
        , m_totalRecv(0) {}

    JNIEnv *m_env;
    jobject m_callback;
    unsigned long m_totalBytes;  // C-STORE: known file size; C-GET: 0 (unknown)

protected:
    void notifySENDProgress(const unsigned long byteCount) override {
        notifyJava(byteCount, m_totalBytes);
        DcmSCU::notifySENDProgress(byteCount);
    }

    // C-GET download: byteCount is cumulative for the CURRENT file being received.
    // It resets to a small value when a new file starts. We detect the reset and
    // accumulate across files to report total bytes received so far.
    void notifyRECEIVEProgress(const unsigned long byteCount) override {
        if (byteCount < m_lastRecvBytes) {
            m_totalRecv += m_lastRecvBytes;
        }
        m_lastRecvBytes = byteCount;
        notifyJava(m_totalRecv + byteCount, 0);
        DcmSCU::notifyRECEIVEProgress(byteCount);
    }

private:
    unsigned long m_lastRecvBytes;  // last byteCount seen for current file
    unsigned long m_totalRecv;      // accumulated bytes from completed files

    void notifyJava(unsigned long sent, unsigned long total) {
        if (m_env && m_callback) {
            jclass cls = m_env->GetObjectClass(m_callback);
            if (cls) {
                jmethodID mid = m_env->GetMethodID(cls, "onProgress", "(JJ)V");
                if (mid) {
                    m_env->CallVoidMethod(m_callback, mid,
                                          (jlong) sent, (jlong) total);
                }
                m_env->DeleteLocalRef(cls);
            }
        }
    }
};

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


static jboolean native_connectPACS(JNIEnv *env, jclass clazz, jstring host, jint port,
                                   jstring local_aet, jstring remote_aet) {
    const char *c_host = env->GetStringUTFChars(host, nullptr);
    const char *c_local_aet = env->GetStringUTFChars(local_aet, nullptr);
    const char *c_remote_aet = env->GetStringUTFChars(remote_aet, nullptr);

    LOGD("native_connectPACS: Attempting to connect to %s:%d (Local: %s, Remote: %s)",
         c_host, port, c_local_aet, c_remote_aet);

    T_ASC_Network *net = nullptr;
    T_ASC_Parameters *params = nullptr;
    T_ASC_Association *assoc = nullptr;
    OFCondition cond = EC_Normal;

    // 1. Initialize Network
    cond = ASC_initializeNetwork(NET_REQUESTOR, 0, 30, &net);
    if (cond.bad()) {
        LOGE("native_connectPACS: Failed to initialize network: %s", cond.text());
        goto cleanup;
    }

    // 2. Create Association Parameters
    cond = ASC_createAssociationParameters(&params,
                                           ASC_DEFAULTMAXPDU, 30);
    if (cond.bad()) {
        LOGE("native_connectPACS: Failed to create association parameters: %s", cond.text());
        goto cleanup;
    }

    // 3. Set Association Parameters
    ASC_setAPTitles(params, c_local_aet, c_remote_aet, nullptr);
    {
        char peer_addr[256];
        snprintf(peer_addr, sizeof(peer_addr), "%s:%d", c_host, (int) port);
        ASC_setPresentationAddresses(params, "localhost", peer_addr);
    }

    // Add a presentation context (e.g., Verification SOP Class / C-ECHO)
    {
        const char *transferSyntaxes[] = {UID_LittleEndianExplicitTransferSyntax};
        cond = ASC_addPresentationContext(params,
                                          1, UID_VerificationSOPClass,
                                          transferSyntaxes, 1);
        if (cond.bad()) {
            LOGE("native_connectPACS: Failed to add presentation context: %s", cond.text());
            goto cleanup;
        }
    }

    // 4. Request Association
    LOGD("native_connectPACS: Requesting Association...");
    cond = ASC_requestAssociation(net, params, &assoc);
    if (cond.bad()) {
        if (cond == DUL_ASSOCIATIONREJECTED) {
            T_ASC_RejectParameters rej;
            ASC_getRejectParameters(params, &rej);
            LOGE("native_connectPACS: Association Rejected: %s", cond.text());
            LOGE("Result: %d, Source: %d, Reason: %d", rej.result, rej.source, rej.reason);
            // Reason 对应含义:
            // 1 - Calling AE Title Not Recognized (Local AET 错了)
            // 3 - Called AE Title Not Recognized  (Remote AET 错了)
        } else {
            LOGE("native_connectPACS: Association Failed: %s", cond.text());
        }
        goto cleanup;
    }

    LOGD("native_connectPACS: Association Established successfully!");

    // 5. Release Association (since we're just testing connection)
    LOGD("native_connectPACS: Releasing Association...");
    cond = ASC_releaseAssociation(assoc);
    if (cond.bad()) {
        LOGE("native_connectPACS: Failed to release association: %s", cond.text());
    }
    ASC_destroyAssociation(&assoc);

    cleanup:
    if (net) ASC_dropNetwork(&net);

    env->ReleaseStringUTFChars(host, c_host);
    env->ReleaseStringUTFChars(local_aet, c_local_aet);
    env->ReleaseStringUTFChars(remote_aet, c_remote_aet);

    return (cond.good()) ? JNI_TRUE : JNI_FALSE;
}

static jboolean native_cEcho(JNIEnv *env, jclass clazz, jstring host, jint port,
                             jstring local_aet, jstring remote_aet) {
    JniString c_host(env, host);
    JniString c_local_aet(env, local_aet);
    JniString c_remote_aet(env, remote_aet);

    LOGD("native_cEcho: %s:%d (L:%s, R:%s)", c_host.c_str(), port, c_local_aet.c_str(),
         c_remote_aet.c_str());

    DcmSCU scu;
    scu.setPeerHostName(c_host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(c_local_aet.c_str());
    scu.setPeerAETitle(c_remote_aet.c_str());

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(UID_VerificationSOPClass, ts);

    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            cond = scu.sendECHORequest(0);
            scu.releaseAssociation();
        }
    }

    LOGD("native_cEcho result: %s", cond.text());
    return cond.good() ? JNI_TRUE : JNI_FALSE;
}

static jboolean native_cStore(JNIEnv *env, jclass clazz, jstring host, jint port,
                              jstring local_aet, jstring remote_aet, jstring dcm_path,
                              jobject callback) {
    JniString c_host(env, host);
    JniString c_local_aet(env, local_aet);
    JniString c_remote_aet(env, remote_aet);
    JniString c_dcm_path(env, dcm_path);

    LOGD("native_cStore: Sending %s to %s:%d", c_dcm_path.c_str(), c_host.c_str(), port);

    DcmFileFormat dfile;
    OFCondition cond = dfile.loadFile(c_dcm_path.c_str());
    if (cond.bad()) {
        LOGE("native_cStore: Failed to load file: %s", cond.text());
        return JNI_FALSE;
    }

    OFString sopClass;
    dfile.getDataset()->findAndGetOFString(DCM_SOPClassUID, sopClass);

    ProgressSCU scu;
    scu.setPeerHostName(c_host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(c_local_aet.c_str());
    scu.setPeerAETitle(c_remote_aet.c_str());

    if (callback != nullptr) {
        struct stat st;
        if (stat(c_dcm_path.c_str(), &st) == 0) {
            scu.m_totalBytes = (unsigned long) st.st_size;
        }
        scu.m_env = env;
        scu.m_callback = callback;
    }

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(sopClass.c_str(), ts);

    cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            T_ASC_PresentationContextID presId =
                    scu.findPresentationContextID(sopClass.c_str(), "");
            if (presId > 0) {
                // 启用自动传输语法转换（DCMTK 会根据协商结果自动转换 dataset）
                scu.setDatasetConversionMode(OFTrue);

                DcmDataset *dataset = dfile.getDataset();
                Uint16 rspStatus = 0;
                // 注意：这里改用发送 dataset 指针，而不是文件路径，以确保发送内存数据
                // 第二个参数是文件名，传空表示直接发送 dataset
                cond = scu.sendSTORERequest(presId, "", dataset, rspStatus);
                LOGD("native_cStore: STORE RSP Status: 0x%04X", rspStatus);
            } else {
                LOGE("native_cStore: No suitable presentation context found for %s",
                     sopClass.c_str());
                cond = EC_TagNotFound;
            }
            scu.releaseAssociation();
        }
    }

    LOGD("native_cStore result: %s", cond.text());
    return cond.good() ? JNI_TRUE : JNI_FALSE;
}

static jobjectArray native_cFind(JNIEnv *env, jclass clazz, jstring host, jint port,
                                 jstring local_aet, jstring remote_aet, jstring patient_name) {
    JniString c_host(env, host);
    JniString c_local_aet(env, local_aet);
    JniString c_remote_aet(env, remote_aet);
    JniString c_pat_name(env, patient_name);

    LOGD("native_cFind: Query for PatientName=%s", c_pat_name.c_str());

    DcmSCU scu;
    scu.setPeerHostName(c_host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(c_local_aet.c_str());
    scu.setPeerAETitle(c_remote_aet.c_str());

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(UID_FINDPatientRootQueryRetrieveInformationModel, ts);

    std::vector<std::string> results;
    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            DcmDataset query;
            query.putAndInsertString(DCM_QueryRetrieveLevel, "PATIENT");
            query.putAndInsertString(DCM_PatientName, c_pat_name.c_str());
            query.putAndInsertString(DCM_PatientID, "");
            query.putAndInsertString(DCM_PatientSex, "");
            query.putAndInsertString(DCM_PatientBirthDate, "");

            T_ASC_PresentationContextID presId = scu.findPresentationContextID(
                    UID_FINDPatientRootQueryRetrieveInformationModel, "");
            if (presId > 0) {
                OFList<QRResponse *> responses;
                cond = scu.sendFINDRequest(presId, &query, &responses);
                if (cond.good()) {
                    for (auto it = responses.begin(); it != responses.end(); ++it) {
                        DcmDataset *ds = (*it)->m_dataset;
                        if (ds) {
                            OFString name, id, sex, birth;
                            ds->findAndGetOFString(DCM_PatientName, name);
                            ds->findAndGetOFString(DCM_PatientID, id);
                            ds->findAndGetOFString(DCM_PatientSex, sex);
                            ds->findAndGetOFString(DCM_PatientBirthDate, birth);

                            std::string res = std::string(name.c_str()) + " | ID:" + id.c_str();
                            if (!sex.empty()) res += " | " + std::string(sex.c_str());
                            if (!birth.empty()) res += " | " + std::string(birth.c_str());
                            results.push_back(res);
                        }
                    }
                }
                for (auto it = responses.begin(); it != responses.end(); ++it) delete *it;
            } else {
                LOGE("native_cFind: No suitable presentation context found");
                cond = EC_TagNotFound;
            }
            scu.releaseAssociation();
        }
    }

    LOGD("native_cFind finished, found %zu results, status: %s", results.size(), cond.text());

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
    JniString c_local_aet(env, local_aet);
    JniString c_remote_aet(env, remote_aet);
    JniString c_pat_id(env, patient_id);
    JniString c_dest_aet(env, dest_aet);

    LOGD("native_cMove: Requesting move of PatID=%s to %s", c_pat_id.c_str(), c_dest_aet.c_str());

    DcmSCU scu;
    scu.setPeerHostName(c_host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(c_local_aet.c_str());
    scu.setPeerAETitle(c_remote_aet.c_str());

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(UID_MOVEPatientRootQueryRetrieveInformationModel, ts);

    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            DcmDataset query;
            query.putAndInsertString(DCM_QueryRetrieveLevel, "PATIENT");
            query.putAndInsertString(DCM_PatientID, c_pat_id.c_str());

            T_ASC_PresentationContextID presId = scu.findPresentationContextID(
                    UID_MOVEPatientRootQueryRetrieveInformationModel, "");
            if (presId > 0) {
                cond = scu.sendMOVERequest(presId, c_dest_aet.c_str(), &query, nullptr);
            } else {
                LOGE("native_cMove: No suitable presentation context found");
                cond = EC_TagNotFound;
            }
            scu.releaseAssociation();
        }
    }

    LOGD("native_cMove result: %s", cond.text());
    return cond.good() ? JNI_TRUE : JNI_FALSE;
}

static jboolean native_cGet(JNIEnv *env, jclass clazz, jstring host, jint port,
                            jstring local_aet, jstring remote_aet, jstring patient_id,
                            jstring save_dir, jobject callback) {
    JniString c_host(env, host);
    JniString c_local_aet(env, local_aet);
    JniString c_remote_aet(env, remote_aet);
    JniString c_pat_id(env, patient_id);
    JniString c_save_dir(env, save_dir);

    LOGD("native_cGet: Requesting GET of PatID=%s to %s", c_pat_id.c_str(), c_save_dir.c_str());

    // 统计下载前 save_dir 中的文件数，用于后续计算实际接收的文件数
    int fileCountBefore = 0;
    {
        DIR *dir = opendir(c_save_dir.c_str());
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_type == DT_REG) fileCountBefore++;
            }
            closedir(dir);
        }
    }
    LOGD("native_cGet: Files in save_dir before C-GET: %d", fileCountBefore);

    ProgressSCU scu;
    scu.setPeerHostName(c_host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(c_local_aet.c_str());
    scu.setPeerAETitle(c_remote_aet.c_str());

    if (callback != nullptr) {
        scu.m_env = env;
        scu.m_callback = callback;
    }

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);

    // C-GET requires Move/Get model
    scu.addPresentationContext(UID_GETPatientRootQueryRetrieveInformationModel, ts);

    // Also need to add storage presentation contexts for what we expect to receive.
    // In C-GET, the SCU acts as an SCP for storage on the same association.
    // We add common storage SOP classes.
    scu.addPresentationContext(UID_SecondaryCaptureImageStorage,
                               ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_ComputedRadiographyImageStorage,
                               ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_CTImageStorage, ts,
                               ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_MRImageStorage, ts,
                               ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_UltrasoundImageStorage, ts,
                               ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_DigitalXRayImageStorageForPresentation, ts,
                               ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_DigitalXRayImageStorageForProcessing, ts,
                               ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_PositronEmissionTomographyImageStorage, ts,
                               ASC_SC_ROLE_SCP);

    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            DcmDataset query;
            query.putAndInsertString(DCM_QueryRetrieveLevel, "PATIENT");
            query.putAndInsertString(DCM_PatientID, c_pat_id.c_str());

            T_ASC_PresentationContextID presId = scu.findPresentationContextID(
                    UID_GETPatientRootQueryRetrieveInformationModel, "");

            if (presId > 0) {
                scu.setStorageDir(c_save_dir.c_str());
                scu.setStorageMode(DCMSCU_STORAGE_DISK);
                OFList<RetrieveResponse *> responses;
                cond = scu.sendCGETRequest(presId, &query, &responses);

                // 打印每个 C-GET 响应的详细信息
                LOGD("native_cGet: Received %zu C-GET responses", responses.size());
                for (auto it = responses.begin(); it != responses.end(); ++it) {
                    RetrieveResponse *rsp = *it;
                    LOGD("native_cGet: RSP status=0x%04X, completed=%d, failed=%d, "
                         "warning=%d, remaining=%d",
                         rsp->m_status,
                         rsp->m_numberOfCompletedSubops,
                         rsp->m_numberOfFailedSubops,
                         rsp->m_numberOfWarningSubops,
                         rsp->m_numberOfRemainingSubops);
                    delete rsp;
                }
            } else {
                LOGE("native_cGet: No suitable presentation context found for C-GET");
                cond = EC_TagNotFound;
            }
            scu.releaseAssociation();
        } else {
            LOGE("native_cGet: Failed to negotiate association: %s", cond.text());
        }
    } else {
        LOGE("native_cGet: Failed to init network: %s", cond.text());
    }

    // 统计下载后 save_dir 中的文件数
    int fileCountAfter = 0;
    {
        DIR *dir = opendir(c_save_dir.c_str());
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_type == DT_REG) fileCountAfter++;
            }
            closedir(dir);
        }
    }
    LOGD("native_cGet: Files in save_dir after C-GET: %d (received %d new files)",
         fileCountAfter, fileCountAfter - fileCountBefore);

    LOGD("native_cGet result: %s", cond.text());
    return cond.good() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Native implementation for DcmtkJni.dcmToJpg(String dir)
 * 将 dir 目录下的所有 DICOM 文件转换为 JPG 图片，输出到 dir/jpg/ 子目录。
 * 转换方式：使用 DicomImage 加载并渲染像素数据（对单色图应用 min/max 窗宽窗位），
 *           再通过 DiJPEGPlugin 写出 JPEG 文件。
 * @return 成功转换的文件数量
 */
static jint native_dcmToJpg(JNIEnv *env, jclass clazz, jstring dir_path) {
    JniString c_dir(env, dir_path);
    if (!c_dir.c_str()) {
        LOGE("native_dcmToJpg: dir is null");
        return 0;
    }
    LOGD("native_dcmToJpg: converting files in %s", c_dir.c_str());

    // 注册 JPEG 解码器，使 DicomImage 能够读取 JPEG 压缩的 DICOM 文件。
    // 注册为全局操作，仅需执行一次。
    static bool codecsRegistered = false;
    if (!codecsRegistered) {
        DJDecoderRegistration::registerCodecs();
        codecsRegistered = true;
        LOGD("native_dcmToJpg: JPEG decoders registered");
    }

    // 创建输出子目录 dir/jpg
    std::string jpgDir = std::string(c_dir.c_str()) + "/jpg";
    if (mkdir(jpgDir.c_str(), 0777) != 0 && errno != EEXIST) {
        LOGE("native_dcmToJpg: failed to create %s: %s", jpgDir.c_str(), strerror(errno));
        return 0;
    }

    DIR *dir = opendir(c_dir.c_str());
    if (!dir) {
        LOGE("native_dcmToJpg: opendir failed for %s: %s", c_dir.c_str(), strerror(errno));
        return 0;
    }

    int converted = 0;
    int failed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        // 仅处理普通文件，跳过子目录（含 jpg 输出目录）
        if (ent->d_type != DT_REG) continue;

        std::string name = ent->d_name;
        // 跳过已经是 jpg 的文件
        if (name.size() >= 4 &&
            name.compare(name.size() - 4, 4, ".jpg") == 0) {
            continue;
        }

        std::string inPath = std::string(c_dir.c_str()) + "/" + name;
        std::string outPath = jpgDir + "/" + name + ".jpg";

        DicomImage img(inPath.c_str());
        if (img.getStatus() != EIS_Normal) {
            LOGW("native_dcmToJpg: failed to load %s: %s", inPath.c_str(),
                 DicomImage::getString(img.getStatus()));
            failed++;
            continue;
        }

        // 单色图像应用自动 min/max 窗宽窗位，改善对比度
        if (img.isMonochrome()) {
            img.setMinMaxWindow();
        }

        DiJPEGPlugin plugin;
        plugin.setQuality(90);
        if (img.writePluginFormat(&plugin, outPath.c_str())) {
            converted++;
            LOGD("native_dcmToJpg: %s -> %s", inPath.c_str(), outPath.c_str());
        } else {
            LOGE("native_dcmToJpg: writePluginFormat failed for %s", inPath.c_str());
            failed++;
        }
    }
    closedir(dir);

    LOGD("native_dcmToJpg: done, converted=%d, failed=%d", converted, failed);
    return converted;
}

// JNI Registration
static const char *const kClassName = "com/example/dcmtkdemo/DcmtkJni";

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
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Lcom/example/dcmtkdemo/ProgressCallback;)Z",
                (void *) native_cStore},
        {"cFind",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;",
                (void *) native_cFind},
        {"cMove",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
                (void *) native_cMove},
        {"cGet",
                "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lcom/example/dcmtkdemo/ProgressCallback;)Z",
                (void *) native_cGet},
        {"dcmToJpg",
                "(Ljava/lang/String;)I",
                (void *) native_dcmToJpg},

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

