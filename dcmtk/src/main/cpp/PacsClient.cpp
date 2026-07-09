#include "dcmtk/config/osconfig.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>
#include <chrono>
#include <set>

#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcxfer.h"
#include "dcmtk/dcmnet/assoc.h"
#include "dcmtk/dcmnet/dimse.h"
#include "dcmtk/dcmnet/scu.h"
#include "dcmtk/dcmjpeg/djdecode.h"
#include "dcmtk/dcmjpeg/djencode.h"
#include "dcmtk/ofstd/ofcond.h"

#include "PacsClient.h"
#include "ProgressScu.h"

#define TAG "DcmtkJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Initialize the static member
std::atomic<bool> PacsClient::m_isCancelled(false);

// Increase PDU size from default 16KB to 64KB for better performance on Android
#define MAX_PDU_SIZE 65536

namespace {
// Adds the three transfer syntaxes we accept for every presentation context.
    void addCommonTransferSyntaxes(OFList<OFString> &ts) {
        ts.push_back(UID_LittleEndianExplicitTransferSyntax);
        ts.push_back(UID_BigEndianExplicitTransferSyntax);
        ts.push_back(UID_LittleEndianImplicitTransferSyntax);
    }
}  // namespace

bool PacsClient::connectPACS(const std::string &host, int port,
                             const std::string &localAet, const std::string &remoteAet) {
    LOGD("native_connectPACS: [START] Verifying connectivity to %s:%d (L:%s, R:%s)",
         host.c_str(), port, localAet.c_str(), remoteAet.c_str());

    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for unstable mobile networks
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(30);

    OFList<OFString> ts;
    ts.push_back(UID_LittleEndianImplicitTransferSyntax);
    scu.addPresentationContext(UID_VerificationSOPClass, ts);

    OFCondition cond = scu.initNetwork();
    if (cond.bad()) {
        LOGE("native_connectPACS: Network init failed: %s", cond.text());
        return false;
    }

    cond = scu.negotiateAssociation();
    if (cond.bad()) {
        if (cond == DUL_ASSOCIATIONREJECTED) {
            LOGE("native_connectPACS: Association REJECTED. Check AE Titles (Local: %s, Remote: %s)",
                 localAet.c_str(), remoteAet.c_str());
        } else {
            LOGE("native_connectPACS: Association failed: %s (Check Host/Port)", cond.text());
        }
        return false;
    }

    LOGD("native_connectPACS: [SUCCESS] Association established and released.");
    scu.releaseAssociation();
    return true;
}

bool PacsClient::cEcho(const std::string &host, int port,
                       const std::string &localAet, const std::string &remoteAet) {
    LOGD("native_cEcho: [START] %s:%d (L:%s, R:%s)", host.c_str(), port, localAet.c_str(),
         remoteAet.c_str());

    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Timeouts for Echo can be relatively short
    scu.setACSETimeout(10);
    scu.setDIMSETimeout(10);

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(UID_VerificationSOPClass, ts);

    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            cond = scu.sendECHORequest(0);
            if (cond.bad()) {
                LOGE("native_cEcho: C-ECHO Request failed: %s", cond.text());
            }
            scu.releaseAssociation();
        } else {
            LOGE("native_cEcho: Association negotiation failed: %s", cond.text());
        }
    } else {
        LOGE("native_cEcho: Network init failed: %s", cond.text());
    }

    LOGD("native_cEcho: [DONE] result: %s", cond.text());
    return cond.good();
}


bool PacsClient::cStore(const std::string &host, int port,
                        const std::string &localAet, const std::string &remoteAet,
                        const std::string &dcmPath, ProgressCallback callback) {
    resetCancel();
    auto startTime = std::chrono::steady_clock::now();
    LOGD("native_cStore: [START] Sending %s to %s:%d", dcmPath.c_str(), host.c_str(), port);

    // 1. 注册编解码器，确保能处理压缩格式
    static bool codecsRegistered = false;
    if (!codecsRegistered) {
        DJDecoderRegistration::registerCodecs();
        DJEncoderRegistration::registerCodecs();
        codecsRegistered = true;
        LOGD("native_cStore: JPEG codecs registered.");
    }

    DcmFileFormat dfile;
    OFCondition cond = dfile.loadFile(dcmPath.c_str());
    if (cond.bad()) {
        LOGE("native_cStore: Failed to load file: %s", cond.text());
        return false;
    }

    // 2. 更稳健地获取 SOP Class UID (优先从 Dataset 找，找不到去 MetaInfo)
    OFString sopClass;
    if (dfile.getDataset()->findAndGetOFString(DCM_SOPClassUID, sopClass).bad() || sopClass.empty()) {
        dfile.getMetaInfo()->findAndGetOFString(DCM_MediaStorageSOPClassUID, sopClass);
    }

    if (sopClass.empty()) {
        LOGE("native_cStore: Could not find SOP Class UID in file.");
        return false;
    }

    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for large file storage
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(60);

    unsigned long fileSize = 0;
    if (callback) {
        struct stat st;
        if (stat(dcmPath.c_str(), &st) == 0) {
            fileSize = (unsigned long) st.st_size;
            scu.setTotalBytes(fileSize);
        }
        scu.setProgressCallback(std::move(callback));
    }

    // 3. 构造传输语法列表
    OFList<OFString> ts;
    // 获取并加入文件原始传输语法 (提高匹配成功率，且能避免不必要的转码)
    E_TransferSyntax xfer = dfile.getDataset()->getOriginalXfer();
    if (xfer != EXS_Unknown) {
        ts.push_back(DcmXfer(xfer).getXferID());
    }
    // 添加其他通用的传输语法作为备选
    addCommonTransferSyntaxes(ts);

    LOGD("native_cStore: Proposing SOP Class %s with %zu transfer syntaxes",
         sopClass.c_str(), ts.size());

    scu.addPresentationContext(sopClass.c_str(), ts);

    cond = scu.initNetwork();
    if (cond.good()) {
        LOGD("native_cStore: Network initialized, negotiating association...");
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            LOGD("native_cStore: Association established.");
            T_ASC_PresentationContextID presId =
                    scu.findPresentationContextID(sopClass.c_str(), "");
            if (presId > 0) {
                scu.setDatasetConversionMode(OFTrue);
                DcmDataset *dataset = dfile.getDataset();
                Uint16 rspStatus = 0;

                LOGD("native_cStore: [SEND_START] Starting C-STORE request (%lu bytes)", fileSize);
                auto storeStart = std::chrono::steady_clock::now();

                cond = scu.sendSTORERequest(presId, "", dataset, rspStatus);

                auto storeEnd = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(storeEnd - storeStart).count();
                LOGD("native_cStore: [SEND_END] Finished in %llds, Status: 0x%04X, Cond: %s",
                     (long long)duration, rspStatus, cond.text());

                if (duration > 30) {
                    LOGW("native_cStore: WARNING: Transmission took %llds (unusually slow)", (long long)duration);
                }
            } else {
                LOGE("native_cStore: No suitable presentation context found for %s",
                     sopClass.c_str());
                cond = EC_TagNotFound;
            }

            LOGD("native_cStore: Releasing association...");
            OFCondition relCond = scu.releaseAssociation();
            if (relCond.bad()) {
                LOGE("native_cStore: Association release failed: %s (Check network/timeout)", relCond.text());
                // 如果传输已成功完成，我们可能不希望因为最后一步释放失败而返回 false
                // 但这里保持原样，仅增加日志。
            }
        } else {
            LOGE("native_cStore: Association negotiation failed: %s", cond.text());
        }
    } else {
        LOGE("native_cStore: Network init failed: %s", cond.text());
    }

    auto totalEndTime = std::chrono::steady_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::seconds>(totalEndTime - startTime).count();
    LOGD("native_cStore: [DONE] Total time: %llds, final status: %s", (long long)totalDuration, cond.text());

    return cond.good();
}

int PacsClient::cStoreMulti(const std::string &host, int port,
                            const std::string &localAet, const std::string &remoteAet,
                            const std::vector<std::string> &dcmPaths,
                            std::function<bool(int index, unsigned long sent, unsigned long total, bool finished, bool success)> callback) {
    resetCancel();
    if (dcmPaths.empty()) return 0;

    LOGD("native_cStoreMulti: [START] Sending %zu files to %s:%d (L:%s, R:%s)",
         dcmPaths.size(), host.c_str(), port, localAet.c_str(), remoteAet.c_str());

    static bool codecsRegistered = false;
    if (!codecsRegistered) {
        DJDecoderRegistration::registerCodecs();
        DJEncoderRegistration::registerCodecs();
        codecsRegistered = true;
    }

    // 1. Scan files to find all unique SOP Classes to negotiate
    std::set<std::string> sopClasses;
    for (const auto &path : dcmPaths) {
        DcmFileFormat dfile;
        if (dfile.loadFile(path.c_str()).good()) {
            OFString sopClass;
            if (dfile.getDataset()->findAndGetOFString(DCM_SOPClassUID,
                                                       sopClass).bad() || sopClass.empty()) {
                dfile.getMetaInfo()->findAndGetOFString(DCM_MediaStorageSOPClassUID, sopClass);
            }
            if (!sopClass.empty()) {
                sopClasses.insert(sopClass.c_str());
            }
        }
    }

    if (sopClasses.empty()) {
        LOGE("native_cStoreMulti: No valid SOP Classes found in files.");
        return 0;
    }

    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for batch operations
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(90);

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);

    for (const auto &sop : sopClasses) {
        scu.addPresentationContext(sop.c_str(), ts);
    }

    int successCount = 0;
    OFCondition cond = scu.initNetwork();
    if (cond.bad()) {
        LOGE("native_cStoreMulti: Network init failed: %s", cond.text());
        return 0;
    }

    cond = scu.negotiateAssociation();
    if (cond.bad()) {
        LOGE("native_cStoreMulti: Association negotiation failed: %s", cond.text());
        return 0;
    }

    // 2. Send each file over the established association
    bool aborted = false;
    for (int i = 0; i < (int) dcmPaths.size(); ++i) {
        if (aborted || isCancelled()) {
            aborted = true;
            break;
        }

        const std::string &path = dcmPaths[i];
        DcmFileFormat dfile;
        if (dfile.loadFile(path.c_str()).bad()) {
            LOGE("native_cStoreMulti: Failed to load file #%d: %s", i, path.c_str());
            if (callback) {
                callback(i, 0, 0, true, false);
            }
            continue;
        }

        OFString sopClass;
        if (dfile.getDataset()->findAndGetOFString(DCM_SOPClassUID, sopClass).bad() || sopClass.empty()) {
            dfile.getMetaInfo()->findAndGetOFString(DCM_MediaStorageSOPClassUID, sopClass);
        }

        T_ASC_PresentationContextID presId = scu.findPresentationContextID(sopClass.c_str(), "");
        if (presId > 0) {
            unsigned long fileSize = 0;
            struct stat st;
            if (stat(path.c_str(), &st) == 0) {
                fileSize = (unsigned long) st.st_size;
                scu.setTotalBytes(fileSize);
            }

            if (callback) {
                // Wrap the multi-callback into the single-file callback expected by ProgressScu
                scu.setProgressCallback([&callback, i, &aborted](unsigned long sent, unsigned long total) {
                    if (!callback(i, sent, total, false, false)) {
                        aborted = true;
                    }
                });
            }

            Uint16 rspStatus = 0;
            OFCondition storeCond = scu.sendSTORERequest(presId, "", dfile.getDataset()
                                                         , rspStatus);

            // 0x0000 = Success, 0xB0xx = Warning (often treated as success in PACS)
            bool success = storeCond.good() && (rspStatus == 0 || (rspStatus & 0xf000) == 0xb000);
            if (success) {
                successCount++;
                LOGD("native_cStoreMulti: File #%d stored successfully (%s)", i, path.c_str());
            } else {
                LOGE("native_cStoreMulti: File #%d storage failed. Status: 0x%04X, Error: %s",
                     i, rspStatus, storeCond.text());
            }

            if (callback) {
                if (!callback(i, fileSize, fileSize, true, success)) {
                    aborted = true;
                }
            }
        } else {
            LOGE("native_cStoreMulti: No negotiated presentation context for SOP Class %s (File #%d)",
                 sopClass.c_str(), i);
            if (callback) {
                callback(i, 0, 0, true, false);
            }
        }
    }

    scu.releaseAssociation();
    LOGD("native_cStoreMulti: [DONE] Successfully stored %d/%zu files.%s",
         successCount, dcmPaths.size(), aborted ? " (Aborted by user)" : "");

    return successCount;
}

std::vector<std::string> PacsClient::cFind(const std::string &host, int port,
                                           const std::string &localAet,
                                           const std::string &remoteAet,
                                           const std::string &patientName) {
    resetCancel();
    LOGD("native_cFind: Query for PatientName=%s", patientName.c_str());

    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for batch operations
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(90);

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
            query.putAndInsertString(DCM_PatientName, patientName.c_str());
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
                            OFString name, id, acc, sex, birth;
                            ds->findAndGetOFString(DCM_PatientName, name);
                            ds->findAndGetOFString(DCM_PatientID, id);
                            ds->findAndGetOFString(DCM_AccessionNumber, acc);
                            ds->findAndGetOFString(DCM_PatientSex, sex);
                            ds->findAndGetOFString(DCM_PatientBirthDate, birth);

                            std::string res = std::string(name.c_str()) + " | ID:" + id.c_str();
                            if (!acc.empty()) res += " | Acc:" + std::string(acc.c_str());
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
    return results;
}

std::vector<std::string> PacsClient::cFindByAccession(const std::string &host, int port,
                                                      const std::string &localAet,
                                                      const std::string &remoteAet,
                                                      const std::string &accessionNumber) {
    resetCancel();
    LOGD("native_cFindByAccession: Query for AccessionNumber=%s", accessionNumber.c_str());

    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for batch operations
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(90);

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    // Use Study Root for Accession Number query as it's more common for that level
    scu.addPresentationContext(UID_FINDStudyRootQueryRetrieveInformationModel, ts);

    std::vector<std::string> results;
    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            DcmDataset query;
            query.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
            query.putAndInsertString(DCM_AccessionNumber, accessionNumber.c_str());
            query.putAndInsertString(DCM_PatientName, "");
            query.putAndInsertString(DCM_PatientID, "");
            query.putAndInsertString(DCM_PatientSex, "");
            query.putAndInsertString(DCM_PatientBirthDate, "");

            T_ASC_PresentationContextID presId = scu.findPresentationContextID(
                    UID_FINDStudyRootQueryRetrieveInformationModel, "");
            if (presId > 0) {
                OFList<QRResponse *> responses;
                cond = scu.sendFINDRequest(presId, &query, &responses);
                if (cond.good()) {
                    for (auto it = responses.begin(); it != responses.end(); ++it) {
                        DcmDataset *ds = (*it)->m_dataset;
                        if (ds) {
                            OFString name, id, acc, sex, birth;
                            ds->findAndGetOFString(DCM_PatientName, name);
                            ds->findAndGetOFString(DCM_PatientID, id);
                            ds->findAndGetOFString(DCM_AccessionNumber, acc);
                            ds->findAndGetOFString(DCM_PatientSex, sex);
                            ds->findAndGetOFString(DCM_PatientBirthDate, birth);

                            std::string res = std::string(name.c_str()) + " | ID:" + id.c_str();
                            if (!acc.empty()) res += " | Acc:" + std::string(acc.c_str());
                            if (!sex.empty()) res += " | " + std::string(sex.c_str());
                            if (!birth.empty()) res += " | " + std::string(birth.c_str());
                            results.push_back(res);
                        }
                    }
                }
                for (auto it = responses.begin(); it != responses.end(); ++it) delete *it;
            } else {
                LOGE("native_cFindByAccession: No suitable presentation context found");
                cond = EC_TagNotFound;
            }
            scu.releaseAssociation();
        }
    }

    LOGD("native_cFindByAccession finished, found %zu results, status: %s", results.size(), cond.text());
    return results;
}

std::vector<DcmDataset*> PacsClient::cFindMWL(const std::string &host, int port,
                                              const std::string &localAet,
                                              const std::string &remoteAet,
                                              const std::string &modality) {
    resetCancel();
    LOGD("native_cFindMWL: [START] Query for Modality=%s", modality.c_str());

    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for batch operations
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(90);

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(UID_FINDModalityWorklistInformationModel, ts);

    std::vector<DcmDataset*> results;
    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        LOGD("native_cFindMWL: Network initialized, negotiating association...");
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            LOGD("native_cFindMWL: Association established.");
            DcmDataset query;
            // Standard MWL return keys at top level
            query.putAndInsertString(DCM_AccessionNumber, "");
            query.putAndInsertString(DCM_PatientName, "");
            query.putAndInsertString(DCM_PatientID, "");
            query.putAndInsertString(DCM_PatientSex, "");
            query.putAndInsertString(DCM_PatientBirthDate, "");
            query.putAndInsertString(DCM_StudyInstanceUID, "");

            // MWL requires ScheduledProcedureStepSequence for filtering and return keys
            DcmItem *spssItem = nullptr;
            query.findOrCreateSequenceItem(DCM_ScheduledProcedureStepSequence, spssItem);
            if (spssItem) {
                // Matching keys
                spssItem->putAndInsertString(DCM_Modality, modality.empty() ? "*" : modality.c_str());
                spssItem->putAndInsertString(DCM_ScheduledProcedureStepStartDate, "");
                spssItem->putAndInsertString(DCM_ScheduledStationAETitle, "");

                // Return keys inside sequence
                spssItem->putAndInsertString(DCM_ScheduledProcedureStepStartTime, "");
                spssItem->putAndInsertString(DCM_ScheduledProcedureStepDescription, "");
            }

            T_ASC_PresentationContextID presId = scu.findPresentationContextID(
                    UID_FINDModalityWorklistInformationModel, "");
            if (presId > 0) {
                OFList<QRResponse *> responses;
                LOGD("native_cFindMWL: Sending C-FIND request...");
                cond = scu.sendFINDRequest(presId, &query, &responses);
                if (cond.good()) {
                    LOGD("native_cFindMWL: Received %zu responses", responses.size());
                    for (auto it = responses.begin(); it != responses.end(); ++it) {
                        DcmDataset *ds = (*it)->m_dataset;
                        if (ds) {
                            // Clone the dataset to take ownership
                            results.push_back(new DcmDataset(*ds));
                        }
                    }
                } else {
                    LOGE("native_cFindMWL: C-FIND request failed: %s", cond.text());
                }
                for (auto it = responses.begin(); it != responses.end(); ++it) delete *it;
            } else {
                LOGE("native_cFindMWL: No suitable presentation context found");
                cond = EC_TagNotFound;
            }
            scu.releaseAssociation();
        } else {
            LOGE("native_cFindMWL: Association negotiation failed: %s", cond.text());
        }
    } else {
        LOGE("native_cFindMWL: Network init failed: %s", cond.text());
    }

    LOGD("native_cFindMWL: [DONE] Found %zu results, status: %s", results.size(), cond.text());
    return results;
}

std::vector<std::string> PacsClient::cFindMWLByTemplate(const std::string &host, int port,
                                                       const std::string &localAet,
                                                       const std::string &remoteAet,
                                                       const std::string &templatePath,
                                                       const std::string &outputDir) {
    resetCancel();
    LOGD("native_cFindMWLByTemplate: [START] Template=%s, OutputDir=%s", templatePath.c_str()
         , outputDir.c_str());

    std::vector<std::string> results;

    // 1. Load template
    DcmFileFormat templateFile;
    OFCondition cond = templateFile.loadFile(templatePath.c_str());
    if (cond.bad()) {
        LOGE("native_cFindMWLByTemplate: Failed to load template: %s", cond.text());
        return results;
    }
    DcmDataset *queryDataset = templateFile.getDataset();

    // 2. Initialize SCU
    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for batch operations
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(90);

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(UID_FINDModalityWorklistInformationModel, ts);

    cond = scu.initNetwork();
    if (cond.bad()) {
        LOGE("native_cFindMWLByTemplate: Network init failed: %s", cond.text());
        return results;
    }

    cond = scu.negotiateAssociation();
    if (cond.bad()) {
        LOGE("native_cFindMWLByTemplate: Association failed: %s", cond.text());
        return results;
    }

    // 3. Send C-FIND and export results
    T_ASC_PresentationContextID presId = scu.findPresentationContextID(
            UID_FINDModalityWorklistInformationModel, "");

    if (presId > 0) {
        OFList<QRResponse *> responses;
        int responseIndex = 0;
        cond = scu.sendFINDRequest(presId, queryDataset, &responses);
        if (cond.bad()) {
            LOGE("native_cFindMWLByTemplate: C-FIND request failed: %s", cond.text());
        } else {
            for (auto it = responses.begin(); it != responses.end(); ++it) {
                DcmDataset *responseDataset = (*it)->m_dataset;
                if (responseDataset != nullptr) {
                    responseIndex++;
                    char fileName[64];
                    sprintf(fileName, "rsp%04d.dcm", responseIndex);
                    std::string outputPath = outputDir + "/" + fileName;

                    DcmFileFormat outFile;
                    // Use copyFrom or assignment to copy the dataset
                    outFile.getDataset()->copyFrom(*responseDataset);

                    OFCondition saveCond = outFile.saveFile(outputPath.c_str(), EXS_LittleEndianExplicit);
                    if (saveCond.good()) {
                        LOGD("native_cFindMWLByTemplate: Exported #%d: %s", responseIndex, outputPath.c_str());
                        results.push_back(outputPath);
                    } else {
                        LOGE("native_cFindMWLByTemplate: Failed to save #%d: %s, Error: %s", responseIndex, outputPath.c_str(), saveCond.text());
                    }
                }
            }
        }
        // Clean up responses
        for (auto it = responses.begin(); it != responses.end(); ++it) delete *it;
    } else {
        LOGE("native_cFindMWLByTemplate: No suitable presentation context found");
    }

    scu.releaseAssociation();
    LOGD("native_cFindMWLByTemplate: [DONE] Exported %d files", (int)results.size());
    return results;
}

bool PacsClient::cMove(const std::string &host, int port,
                       const std::string &localAet, const std::string &remoteAet,
                       const std::string &patientId, const std::string &destAet) {
    resetCancel();
    LOGD("native_cMove: Requesting move of PatID=%s to %s", patientId.c_str(), destAet.c_str());

    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for batch operations
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(90);

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(UID_MOVEPatientRootQueryRetrieveInformationModel, ts);

    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            DcmDataset query;
            query.putAndInsertString(DCM_QueryRetrieveLevel, "PATIENT");
            query.putAndInsertString(DCM_PatientID, patientId.c_str());

            T_ASC_PresentationContextID presId = scu.findPresentationContextID(
                    UID_MOVEPatientRootQueryRetrieveInformationModel, "");
            if (presId > 0) {
                cond = scu.sendMOVERequest(presId, destAet.c_str(), &query, nullptr);
            } else {
                LOGE("native_cMove: No suitable presentation context found");
                cond = EC_TagNotFound;
            }
            scu.releaseAssociation();
        }
    }

    LOGD("native_cMove result: %s", cond.text());
    return cond.good();
}

bool PacsClient::cGet(const std::string &host, int port,
                      const std::string &localAet, const std::string &remoteAet,
                      const std::string &patientId, const std::string &saveDir,
                      ProgressCallback callback) {
    LOGD("native_cGet: Requesting GET of PatID=%s to %s", patientId.c_str(), saveDir.c_str());

    // 统计下载前 save_dir 中的文件数，用于后续计算实际接收的文件数
    int fileCountBefore = 0;
    {
        DIR *dir = opendir(saveDir.c_str());
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_type == DT_REG) fileCountBefore++;
            }
            closedir(dir);
        }
    }
    LOGD("native_cGet: Files in save_dir before C-GET: %d", fileCountBefore);

    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    // Increase timeouts for batch operations
    scu.setACSETimeout(30);
    scu.setDIMSETimeout(90);

    if (callback) {
        scu.setProgressCallback(std::move(callback));
    }

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);

    // C-GET requires Move/Get model
    scu.addPresentationContext(UID_GETPatientRootQueryRetrieveInformationModel, ts);

    // Also need to add storage presentation contexts for what we expect to receive.
    // In C-GET, the SCU acts as an SCP for storage on the same association.
    // We add common storage SOP classes.
    scu.addPresentationContext(UID_SecondaryCaptureImageStorage, ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_ComputedRadiographyImageStorage, ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_CTImageStorage, ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_MRImageStorage, ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_UltrasoundImageStorage, ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_DigitalXRayImageStorageForPresentation, ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_DigitalXRayImageStorageForProcessing, ts, ASC_SC_ROLE_SCP);
    scu.addPresentationContext(UID_PositronEmissionTomographyImageStorage, ts, ASC_SC_ROLE_SCP);

    OFCondition cond = scu.initNetwork();
    if (cond.good()) {
        cond = scu.negotiateAssociation();
        if (cond.good()) {
            DcmDataset query;
            query.putAndInsertString(DCM_QueryRetrieveLevel, "PATIENT");
            query.putAndInsertString(DCM_PatientID, patientId.c_str());

            T_ASC_PresentationContextID presId = scu.findPresentationContextID(
                    UID_GETPatientRootQueryRetrieveInformationModel, "");

            if (presId > 0) {
                scu.setStorageDir(saveDir.c_str());
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
        DIR *dir = opendir(saveDir.c_str());
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
    return cond.good();
}
