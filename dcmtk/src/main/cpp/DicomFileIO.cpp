#include "dcmtk/config/osconfig.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

    // Fix: Convert to UTF-8 to avoid JNI NewStringUTF crash with non-UTF8 characters
    dataset->convertToUTF8();

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

        if (size <= 0) {
            LOGE("native_writeDicomFile: Invalid raw file size: %ld", size);
            fclose(f);
            return false;
        }

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
        size_t numPixels = size / 2;

        if (numPixels > 0) {
            for (size_t i = 0; i < numPixels; ++i) {
                // 显式按大端序读取值用于计算
                uint8_t high = pixelData[2 * i];
                uint8_t low  = pixelData[2 * i + 1];
                Uint16 val = (Uint16)((high << 8) | low);

                if (val < minVal) minVal = val;
                if (val > maxVal) maxVal = val;
            }

            double windowWidth = maxVal - minVal;
            double windowCenter = minVal + (windowWidth / 2.0);
            // ... (rest of window calculation)
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
            // 重新按大端读取并存入 native 数组，以确保 DCMTK 写入正确的 DICOM 字节流
            std::vector<Uint16> swappedData(numPixels);
            for (size_t i = 0; i < numPixels; ++i) {
                uint8_t high = pixelData[2 * i];
                uint8_t low  = pixelData[2 * i + 1];
                swappedData[i] = (Uint16)((high << 8) | low);
            }
            dataset->putAndInsertUint16Array(DCM_PixelData, swappedData.data(), (Uint32)numPixels);
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

// =============================================================================
// 对应 dcm4che3 版 DicomFileUtils.kt 的新增方法
// =============================================================================

namespace {
// 取 dataset/item 中某 tag 的字符串值，缺失返回 default
    std::string getStr(DcmItem *item, const DcmTagKey &key, const char *def = "") {
        OFString s;
        if (item && item->findAndGetOFString(key, s).good() && s.length() > 0) {
            return s.c_str();
        }
        return def;
    }

// 按 '\' 拆分多值字符串
    std::vector<std::string> splitBackslash(const std::string &s) {
        std::vector<std::string> out;
        if (s.empty()) return out;
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(s[i]);
            }
        }
        out.push_back(cur);
        return out;
    }
} // namespace

std::map<std::string, std::string> DicomFileIO::loadFileInfoNamed(const std::string &filePath) {
    LOGD("loadFileInfoNamed: %s", filePath.c_str());
    std::map<std::string, std::string> result;

    DcmFileFormat ff;
    if (ff.loadFile(filePath.c_str()).bad()) {
        LOGE("loadFileInfoNamed: loadFile failed: %s", filePath.c_str());
        return result;
    }
    DcmDataset *ds = ff.getDataset();
    ds->loadAllDataIntoMemory();

    // Fix: Convert to UTF-8
    ds->convertToUTF8();

    result["ExposureIndex"] = getStr(ds, DCM_ExposureIndex);
    result["PatientName"] = getStr(ds, DCM_PatientName);
    result["PatientBirthDate"] = getStr(ds, DCM_PatientBirthDate);
    result["InstitutionName"] = getStr(ds, DCM_InstitutionName);

    // StudyDate + StudyTime -> "YYYY-MM-DD HH:MM:SS"
    std::string studyDate = getStr(ds, DCM_StudyDate);
    std::string studyTime = getStr(ds, DCM_StudyTime);
    if (!studyTime.empty()) studyTime.resize(6, '0'); // padEnd(6,'0')
    if (studyDate.size() == 8 && studyTime.size() >= 6) {
        result["StudyDate"] = studyDate.substr(0, 4) + "-" + studyDate.substr(4, 2) + "-" +
                              studyDate.substr(6, 2) + " " + studyTime.substr(0, 2) + ":" +
                              studyTime.substr(2, 2) + ":" + studyTime.substr(4, 2);
    } else {
        result["StudyDate"] = "0000-00-00 00:00:00";
    }

    result["PatientID"] = getStr(ds, DCM_PatientID);
    result["StudyID"] = getStr(ds, DCM_StudyID);
    result["PatientAge"] = getStr(ds, DCM_PatientAge);

    std::string sex = getStr(ds, DCM_PatientSex);
    if (sex == "M") result["PatientSex"] = "男";
    else if (sex == "F") result["PatientSex"] = "女";
    else result["PatientSex"] = "";

    result["BodyPartExamined"] = getStr(ds, DCM_BodyPartExamined);

    LOGD("loadFileInfoNamed: done, %zu keys", result.size());
    return result;
}

std::map<std::string, std::string> DicomFileIO::readWindowSettings(const std::string &filePath) {
    LOGD("readWindowSettings: %s", filePath.c_str());
    std::map<std::string, std::string> result;

    DcmFileFormat ff;
    if (ff.loadFile(filePath.c_str()).bad()) {
        LOGE("readWindowSettings: loadFile failed: %s", filePath.c_str());
        return result;
    }
    DcmDataset *ds = ff.getDataset();
    // Fix: Convert to UTF-8
    ds->convertToUTF8();

    Uint16 smallest = 0, largest = 4095;
    ds->findAndGetUint16(DCM_SmallestImagePixelValue, smallest);
    ds->findAndGetUint16(DCM_LargestImagePixelValue, largest);

    result["smallestPixelValue"] = std::to_string(smallest);
    result["largestPixelValue"] = std::to_string(largest);

    double autoCenter = (smallest + largest) / 2.0;
    double autoWidth = (double) largest - (double) smallest;
    if (autoWidth < 1.0) autoWidth = 1.0;
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", autoCenter);
        result["autoCenter"] = buf;
        snprintf(buf, sizeof(buf), "%.6f", autoWidth);
        result["autoWidth"] = buf;
    }

    // 优先标准 WindowCenter/WindowWidth
    std::vector<std::string> centers = splitBackslash(getStr(ds, DCM_WindowCenter));
    std::vector<std::string> widths = splitBackslash(getStr(ds, DCM_WindowWidth));
    std::vector<std::string> descs = splitBackslash(getStr(ds, DCM_WindowCenterWidthExplanation));

    std::vector<std::string> winCenters, winWidths, winDescs;
    if (!centers.empty() && !widths.empty()) {
        size_t count = std::min(centers.size(), widths.size());
        for (size_t i = 0; i < count; ++i) {
            winCenters.push_back(centers[i]);
            winWidths.push_back(widths[i]);
            winDescs.push_back(i < descs.size() ? descs[i] : "");
        }
    } else {
        // 回退 VOI LUT Sequence
        DcmSequenceOfItems *seq = nullptr;
        if (ds->findAndGetSequence(DCM_VOILUTSequence, seq).good() && seq) {
            unsigned long card = seq->card();
            for (unsigned long i = 0; i < card; ++i) {
                DcmItem *item = seq->getItem(i);
                if (!item) continue;
                std::vector<std::string> lutDesc = splitBackslash(getStr(item, DCM_LUTDescriptor));
                if (lutDesc.size() >= 3) {
                    long numEntries = atol(lutDesc[0].c_str());
                    long firstInput = atol(lutDesc[1].c_str());
                    double w = (double) numEntries;
                    double c = firstInput + w / 2.0;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.6f", c);
                    winCenters.push_back(buf);
                    snprintf(buf, sizeof(buf), "%.6f", w);
                    winWidths.push_back(buf);
                    winDescs.push_back(getStr(item, DCM_LUTExplanation));
                }
            }
        }
    }

    result["windowCount"] = std::to_string(winCenters.size());
    for (size_t i = 0; i < winCenters.size(); ++i) {
        char key[64];
        snprintf(key, sizeof(key), "window_%zu_center", i);
        result[key] = winCenters[i];
        snprintf(key, sizeof(key), "window_%zu_width", i);
        result[key] = winWidths[i];
        snprintf(key, sizeof(key), "window_%zu_desc", i);
        result[key] = winDescs[i];
    }

    LOGD("readWindowSettings: done, windows=%zu", winCenters.size());
    return result;
}

bool DicomFileIO::dicomFileToBitmapRgba(const std::string &filePath, bool useCustomWindow,
                                        double ww, double wc,
                                        std::vector<uint8_t> &outRgba,
                                        int &outW, int &outH) {
    LOGD("dicomFileToBitmapRgba: %s custom=%d ww=%.2f wc=%.2f",
         filePath.c_str(), useCustomWindow ? 1 : 0, ww, wc);

    DicomImage img(filePath.c_str());
    if (img.getStatus() != EIS_Normal) {
        LOGE("dicomFileToBitmapRgba: DicomImage load failed: %s",
             DicomImage::getString(img.getStatus()));
        return false;
    }

    int w = (int) img.getWidth();
    int h = (int) img.getHeight();
    if (w <= 0 || h <= 0) {
        LOGE("dicomFileToBitmapRgba: invalid dims %dx%d", w, h);
        return false;
    }

    if (useCustomWindow) {
        if (ww < 1.0 || !img.setWindow(wc, ww)) {
            LOGW("dicomFileToBitmapRgba: setWindow(custom) failed, fallback minmax");
            img.setMinMaxWindow();
        }
    } else {
        // 显式读取文件自带 WindowCenter/WindowWidth，匹配 dcm4che 自行解析行为
        DcmFileFormat ff;
        if (ff.loadFile(filePath.c_str()).good()) {
            DcmDataset *ds = ff.getDataset();
            std::string wcStr = getStr(ds, DCM_WindowCenter);
            std::string wwStr = getStr(ds, DCM_WindowWidth);
            std::vector<std::string> wcs = splitBackslash(wcStr);
            std::vector<std::string> wws = splitBackslash(wwStr);
            if (!wcs.empty() && !wws.empty()) {
                double c = atof(wcs[0].c_str());
                double wd = atof(wws[0].c_str());
                if (wd >= 1.0 && img.setWindow(c, wd)) {
                    // applied
                } else {
                    img.setMinMaxWindow();
                }
            } else {
                img.setMinMaxWindow();
            }
        } else {
            img.setMinMaxWindow();
        }
    }

    const void *data = img.getOutputData(8, 0);
    if (!data) {
        LOGE("dicomFileToBitmapRgba: getOutputData(8) returned null");
        return false;
    }

    const uint8_t *gray = (const uint8_t *) data;
    size_t total = (size_t) w * (size_t) h;
    outRgba.resize(total * 4);
    for (size_t i = 0; i < total; ++i) {
        uint8_t v = gray[i];
        size_t o = i * 4;
        outRgba[o] = v;
        outRgba[o + 1] = v;
        outRgba[o + 2] = v;
        outRgba[o + 3] = 0xFF;
    }
    img.deleteOutputData();

    outW = w;
    outH = h;
    LOGD("dicomFileToBitmapRgba: ok %dx%d, rgba=%zu bytes", w, h, outRgba.size());
    return true;
}

bool DicomFileIO::writeDcmFileFull(const std::string &dcmPath, const ScanRecordInfo &record,
                                   const PixelDataInfo &pixelData) {
    LOGD("writeDcmFileFull: dcm=%s %dx%d", dcmPath.c_str(), pixelData.columns, pixelData.rows);

    DcmFileFormat ff;
    DcmDataset *ds = ff.getDataset();

    ds->putAndInsertString(DCM_SpecificCharacterSet, "ISO_IR 192"); // UTF-8
    ds->putAndInsertString(DCM_InstitutionName, "momo");
    ds->putAndInsertString(DCM_Manufacturer, "VRN");
    ds->putAndInsertString(DCM_ManufacturerModelName, "EQ800");

    ds->putAndInsertString(DCM_PatientID, std::to_string(record.examineNo).c_str());
    ds->putAndInsertString(DCM_PatientName, record.patientName.c_str());
    ds->putAndInsertString(DCM_PatientAge, record.patientAge.c_str());
    const char *sexCode = (record.patientSex == "男") ? "M" :
                          (record.patientSex == "女") ? "F" : "O";
    ds->putAndInsertString(DCM_PatientSex, sexCode);

    ds->putAndInsertString(DCM_StudyID, std::to_string(record.examineNo).c_str());

    std::time_t now = std::time(nullptr);
    std::tm *lt = std::localtime(&now);
    char dateBuf[16], timeBuf[16];
    snprintf(dateBuf, sizeof(dateBuf), "%04d%02d%02d", lt->tm_year + 1900, lt->tm_mon + 1,
             lt->tm_mday);
    snprintf(timeBuf, sizeof(timeBuf), "%02d%02d%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
    ds->putAndInsertString(DCM_StudyDate, dateBuf);
    ds->putAndInsertString(DCM_StudyTime, timeBuf);

    ds->putAndInsertString(DCM_Modality, "CR");
    ds->putAndInsertString(DCM_BodyPartExamined, record.toothPosition.c_str());
    ds->putAndInsertString(DCM_SeriesNumber, "1");
    ds->putAndInsertString(DCM_InstanceNumber, "1");
    ds->putAndInsertString(DCM_ImageType, "ORIGINAL\\PRIMARY");

    ds->putAndInsertUint16(DCM_Rows, (Uint16) pixelData.rows);
    ds->putAndInsertUint16(DCM_Columns, (Uint16) pixelData.columns);
    ds->putAndInsertUint16(DCM_SamplesPerPixel, 1);
    // 修复：CT 标准通常使用 MONOCHROME2 (0为黑)
    ds->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
    ds->putAndInsertUint16(DCM_BitsAllocated, 16);
    ds->putAndInsertUint16(DCM_BitsStored, 16);
    ds->putAndInsertUint16(DCM_HighBit, 15);
    ds->putAndInsertUint16(DCM_PixelRepresentation, 0);

    // 修复：写入 Rescale 标签，使 Pixel Data 的原始值能正确映射回 HU 值
    // HU = PixelValue * RescaleSlope + RescaleIntercept
    ds->putAndInsertString(DCM_RescaleSlope, "1.0");
    ds->putAndInsertString(DCM_RescaleIntercept, "-1024.0");
    ds->putAndInsertString(DCM_RescaleType, "HU");

    {
        char ps[64];
        snprintf(ps, sizeof(ps), "%.8f\\%.8f", 0.03369563, 0.03346939);
        ds->putAndInsertString(DCM_PixelSpacing, ps);
        ds->putAndInsertString(DCM_ImagerPixelSpacing, ps);
    }

    // PixelData（16-bit）
    size_t rd = pixelData.data.size();
    if (rd % 2 == 0) {
        Uint32 numWords = (Uint32)(rd / 2);
        std::vector<Uint16> swappedData(numWords);
        const uint8_t* rawPtr = pixelData.data.data();

        for (Uint32 i = 0; i < numWords; ++i) {
            // 显式按大端序（高位在前）组合字节
            uint8_t high = rawPtr[2 * i];
            uint8_t low  = rawPtr[2 * i + 1];
            swappedData[i] = (Uint16)((high << 8) | low);
        }
        ds->putAndInsertUint16Array(DCM_PixelData, swappedData.data(), numWords);
    } else {
        ds->putAndInsertUint8Array(DCM_PixelData, pixelData.data.data(), (Uint32) rd);
    }

    {
        char w[32], c[32];
        snprintf(w, sizeof(w), "%d", pixelData.win_width);
        snprintf(c, sizeof(c), "%d", pixelData.win_center);
        ds->putAndInsertString(DCM_WindowWidth, w);
        ds->putAndInsertString(DCM_WindowCenter, c);
    }
    {
        char e[32];
        snprintf(e, sizeof(e), "%d", pixelData.exposure_leve);
        ds->putAndInsertString(DCM_ExposureIndex, e);
    }
    ds->putAndInsertString(DCM_TargetExposureIndex, "28000");
    ds->putAndInsertString(DCM_DeviationIndex, "1000");
    ds->putAndInsertUint16(DCM_LargestImagePixelValue, (Uint16) pixelData.largestImagePixelValue);

    ds->putAndInsertString(DCM_SoftwareVersions, "DCMTK 3.6.9");
    ds->putAndInsertString(DCM_StationName, "VRN-EQ800");
    ds->putAndInsertString(DCM_StudyDescription, "Dental X-Ray");
    ds->putAndInsertString(DCM_SeriesDescription, "Tooth Region Scan");
    ds->putAndInsertString(DCM_PositionReferenceIndicator, "HFS");

    char sopUid[100], studyUid[100], seriesUid[100];
    dcmGenerateUniqueIdentifier(sopUid, SITE_INSTANCE_UID_ROOT);
    dcmGenerateUniqueIdentifier(studyUid, SITE_STUDY_UID_ROOT);
    dcmGenerateUniqueIdentifier(seriesUid, SITE_SERIES_UID_ROOT);
    ds->putAndInsertString(DCM_SOPClassUID, UID_ComputedRadiographyImageStorage);
    ds->putAndInsertString(DCM_SOPInstanceUID, sopUid);
    ds->putAndInsertString(DCM_StudyInstanceUID, studyUid);
    ds->putAndInsertString(DCM_SeriesInstanceUID, seriesUid);

    OFCondition st = ff.saveFile(dcmPath.c_str(), EXS_LittleEndianExplicit);
    if (st.bad()) {
        LOGE("writeDcmFileFull: saveFile failed: %s", st.text());
        return false;
    }
    LOGD("writeDcmFileFull: saved %s", dcmPath.c_str());

    return true;
}
