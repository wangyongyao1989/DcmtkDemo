#ifndef DCMTKDEMO_DICOMFILEIO_H
#define DCMTKDEMO_DICOMFILEIO_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// DICOM file operations: tag reading, raw->DICOM writing, and DICOM->JPG
// conversion. Pure C++ interface, independent of JNI.
class DicomFileIO {
public:
    // Load every leaf element of a DICOM file into a map keyed by "(grp,elem)".
    // Returns an empty map on failure (load error / null path).
    static std::map<std::string, std::string> loadFileInfo(const std::string &filePath);

    // Build a DICOM file from a raw 16-bit pixel buffer at `rawPath`, writing
    // the result to `dcmPath`. Computes min/max window center/width. Returns
    // true on success.
    static bool writeDicomFile(const std::string &rawPath, const std::string &dcmPath,
                               int width, int height);

    // Convert every non-JPG file in `dir` to `dir/jpg/<name>.jpg` using min/max
    // windowing for monochrome images. Returns the number of files converted.
    static int dcmToJpg(const std::string &dir);

    // ----------------------------------------------------------------------
    // 以下方法对应 dcm4che3 版 DicomFileUtils.kt 中的同名功能（新增，不影响既有方法）
    // ----------------------------------------------------------------------

    // 对应 loadDicomFileInfo(file): 返回命名 key 的信息 map。
    // key: ExposureIndex, PatientName, PatientBirthDate, InstitutionName,
    //      StudyDate(YYYY-MM-DD HH:MM:SS), PatientID, StudyID, PatientAge,
    //      PatientSex(M->男/F->女/else->""), BodyPartExamined
    static std::map<std::string, std::string> loadFileInfoNamed(const std::string &filePath);

    // 对应 readDicomWindowSettings(file): 返回扁平 map，由上层 Kotlin 组装为
    // DicomWindowSettings。key: smallestPixelValue, largestPixelValue, autoCenter,
    // autoWidth, windowCount, window_N_center/width/desc。
    static std::map<std::string, std::string> readWindowSettings(const std::string &filePath);

    // 对应 dicomFile2Bitmap(file[,ww,wc]): 将 DICOM 渲染为 8-bit 灰度并展开为
    // RGBA8888（每像素 R=G=B=v, A=0xFF）。useCustomWindow=false 时使用文件自带
    // WindowCenter/WindowWidth；=true 时使用传入 ww/wc。成功返回 true。
    static bool dicomFileToBitmapRgba(const std::string &filePath, bool useCustomWindow,
                                      double ww, double wc,
                                      std::vector<uint8_t> &outRgba,
                                      int &outW, int &outH);

    // ScanRecord 入参（对应 Kotlin ScanRecord）
    struct ScanRecordInfo {
        int examineNo;
        std::string patientName;
        std::string patientAge;
        std::string patientSex;   // "男"/"女"/其它
        std::string toothPosition;
    };

    // PixelData 出参（对应 Kotlin PixelData）
    struct PixelDataInfo {
        int rows;
        int columns;
        std::vector<uint8_t> data;   // 16-bit 像素字节（小端）
        double win_width;
        double win_center;
        int exposure_leve;
        int largestImagePixelValue;
    };

    // 对应 writeDcmFile(record, rawFile, dcmFile, w, h): 写完整 CR DICOM，
    // 返回处理后的 PixelData 信息。成功返回 true。
    static bool writeDcmFileFull(const std::string &rawPath, const std::string &dcmPath,
                                 int width, int height, const ScanRecordInfo &record,
                                 PixelDataInfo &outPixelData);
};

#endif // DCMTKDEMO_DICOMFILEIO_H
