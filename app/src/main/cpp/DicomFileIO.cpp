#include "dcmtk/config/osconfig.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <string>

#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmimgle/dcmimage.h"
#include "dcmtk/dcmjpeg/dipijpeg.h"
#include "dcmtk/dcmjpeg/djdecode.h"

#include "DicomFileIO.h"

#define TAG "DcmtkJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

std::map<std::string, std::string> DicomFileIO::loadFileInfo(const std::string &filePath) {
    LOGD("native_loadDicomFileInfo: Entering with path: %s", filePath.c_str());

    std::map<std::string, std::string> result;

    DcmFileFormat fileformat;
    OFCondition status = fileformat.loadFile(filePath.c_str());
    LOGD("native_loadDicomFileInfo: loadFile status: %s", status.text());

    if (status.bad()) {
        LOGE("native_loadDicomFileInfo: Failed to load DICOM: %s", status.text());
        return result;
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

                result[tagStr] = valueStr.c_str();
                elementCount++;
            }
        }
    }

    LOGD("native_loadDicomFileInfo: Finished processing, found %d leaf elements", elementCount);
    return result;
}

bool DicomFileIO::writeDicomFile(const std::string &rawPath, const std::string &dcmPath,
                                 int width, int height) {
    LOGD("native_writeDicomFile: src=%s, dest=%s, w=%d, h=%d",
         rawPath.c_str(), dcmPath.c_str(), width, height);

    bool success = false;

    FILE *f = fopen(rawPath.c_str(), "rb");
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

        LOGD("native_writeDicomFile: Saving file to: %s", dcmPath.c_str());
        OFCondition status = fileformat.saveFile(dcmPath.c_str(), EXS_LittleEndianExplicit);
        if (status.good()) {
            success = true;
            LOGD("native_writeDicomFile: Successfully wrote DICOM");
        } else {
            LOGE("native_writeDicomFile: Failed to save DICOM: %s", status.text());
        }

        delete[] pixelData;
    } else {
        LOGE("native_writeDicomFile: Failed to open source raw file: %s, errno: %d (%s)",
             rawPath.c_str(), errno, strerror(errno));
    }

    return success;
}

int DicomFileIO::dcmToJpg(const std::string &dir) {
    if (dir.empty()) {
        LOGE("native_dcmToJpg: dir is null");
        return 0;
    }
    LOGD("native_dcmToJpg: converting files in %s", dir.c_str());

    // 注册 JPEG 解码器，使 DicomImage 能够读取 JPEG 压缩的 DICOM 文件。
    // 注册为全局操作，仅需执行一次。
    static bool codecsRegistered = false;
    if (!codecsRegistered) {
        DJDecoderRegistration::registerCodecs();
        codecsRegistered = true;
        LOGD("native_dcmToJpg: JPEG decoders registered");
    }

    // 创建输出子目录 dir/jpg
    std::string jpgDir = dir + "/jpg";
    if (mkdir(jpgDir.c_str(), 0777) != 0 && errno != EEXIST) {
        LOGE("native_dcmToJpg: failed to create %s: %s", jpgDir.c_str(), strerror(errno));
        return 0;
    }

    DIR *d = opendir(dir.c_str());
    if (!d) {
        LOGE("native_dcmToJpg: opendir failed for %s: %s", dir.c_str(), strerror(errno));
        return 0;
    }

    int converted = 0;
    int failed = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        // 仅处理普通文件，跳过子目录（含 jpg 输出目录）
        if (ent->d_type != DT_REG) continue;

        std::string name = ent->d_name;
        // 跳过已经是 jpg 的文件
        if (name.size() >= 4 &&
            name.compare(name.size() - 4, 4, ".jpg") == 0) {
            continue;
        }

        std::string inPath = dir + "/" + name;
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
    closedir(d);

    LOGD("native_dcmToJpg: done, converted=%d, failed=%d", converted, failed);
    return converted;
}
