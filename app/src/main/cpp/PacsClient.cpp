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

#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmnet/assoc.h"
#include "dcmtk/dcmnet/dimse.h"
#include "dcmtk/dcmnet/scu.h"
#include "dcmtk/ofstd/ofcond.h"

#include "PacsClient.h"
#include "ProgressScu.h"

#define TAG "DcmtkJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

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
    const char *c_host = host.c_str();
    const char *c_local_aet = localAet.c_str();
    const char *c_remote_aet = remoteAet.c_str();

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
    cond = ASC_createAssociationParameters(&params, MAX_PDU_SIZE, 30);
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
        cond = ASC_addPresentationContext(params, 1, UID_VerificationSOPClass,
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

    return cond.good();
}

bool PacsClient::cEcho(const std::string &host, int port,
                       const std::string &localAet, const std::string &remoteAet) {
    LOGD("native_cEcho: %s:%d (L:%s, R:%s)", host.c_str(), port, localAet.c_str(),
         remoteAet.c_str());

    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

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
    return cond.good();
}

bool PacsClient::cStore(const std::string &host, int port,
                        const std::string &localAet, const std::string &remoteAet,
                        const std::string &dcmPath, ProgressCallback callback) {
    auto startTime = std::chrono::steady_clock::now();
    LOGD("native_cStore: [START] Sending %s to %s:%d", dcmPath.c_str(), host.c_str(), port);

    DcmFileFormat dfile;
    OFCondition cond = dfile.loadFile(dcmPath.c_str());
    if (cond.bad()) {
        LOGE("native_cStore: Failed to load file: %s", cond.text());
        return false;
    }

    OFString sopClass;
    dfile.getDataset()->findAndGetOFString(DCM_SOPClassUID, sopClass);

    ProgressScu scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    unsigned long fileSize = 0;
    if (callback) {
        struct stat st;
        if (stat(dcmPath.c_str(), &st) == 0) {
            fileSize = (unsigned long) st.st_size;
            scu.setTotalBytes(fileSize);
        }
        scu.setProgressCallback(std::move(callback));
    }

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
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

std::vector<std::string> PacsClient::cFind(const std::string &host, int port,
                                           const std::string &localAet,
                                           const std::string &remoteAet,
                                           const std::string &patientName) {
    LOGD("native_cFind: Query for PatientName=%s", patientName.c_str());

    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

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
    LOGD("native_cFindByAccession: Query for AccessionNumber=%s", accessionNumber.c_str());

    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

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

std::vector<std::string> PacsClient::cFindMWL(const std::string &host, int port,
                                              const std::string &localAet,
                                              const std::string &remoteAet,
                                              const std::string &modality) {
    LOGD("native_cFindMWL: [START] Query for Modality=%s", modality.c_str());

    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

    OFList<OFString> ts;
    addCommonTransferSyntaxes(ts);
    scu.addPresentationContext(UID_FINDModalityWorklistInformationModel, ts);

    std::vector<std::string> results;
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
                            OFString name, id, acc, sex, birth, studyUid, mod, spsDesc;
                            ds->findAndGetOFString(DCM_PatientName, name);
                            ds->findAndGetOFString(DCM_PatientID, id);
                            ds->findAndGetOFString(DCM_AccessionNumber, acc);
                            ds->findAndGetOFString(DCM_PatientSex, sex);
                            ds->findAndGetOFString(DCM_PatientBirthDate, birth);
                            ds->findAndGetOFString(DCM_StudyInstanceUID, studyUid);

                            // Modality and other details are inside the sequence
                            DcmItem *item = nullptr;
                            if (ds->findAndGetSequenceItem(DCM_ScheduledProcedureStepSequence, item, 0).good()) {
                                item->findAndGetOFString(DCM_Modality, mod);
                                item->findAndGetOFString(DCM_ScheduledProcedureStepDescription, spsDesc);
                            }

                            // Format: "Name | ID:id | Acc:acc | Sex:sex | Birth:birth | Mod:mod | Desc:desc"
                            std::string res = std::string(name.c_str()) + " | ID:" + id.c_str();
                            if (!acc.empty()) res += " | Acc:" + std::string(acc.c_str());
                            if (!sex.empty()) res += " | Sex:" + std::string(sex.c_str());
                            if (!birth.empty()) res += " | Birth:" + std::string(birth.c_str());
                            if (!mod.empty()) res += " | Mod:" + std::string(mod.c_str());
                            if (!spsDesc.empty()) res += " | Desc:" + std::string(spsDesc.c_str());

                            LOGD("native_cFindMWL: Found record: %s", res.c_str());
                            results.push_back(res);
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
    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

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
    LOGD("native_cMove: Requesting move of PatID=%s to %s", patientId.c_str(), destAet.c_str());

    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(port);
    scu.setAETitle(localAet.c_str());
    scu.setPeerAETitle(remoteAet.c_str());
    scu.setMaxReceivePDULength(MAX_PDU_SIZE);

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
