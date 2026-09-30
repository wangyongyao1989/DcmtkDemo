#ifndef DCMTKDEMO_SRREPORT_H
#define DCMTKDEMO_SRREPORT_H

#include <string>
#include <vector>

#include "include/MeasureTypes.h"

/** SR 文档的患者 / Study 级信息（PRD 5.5.2 报告内容结构的"患者信息"章节） */
struct SrPatientInfo {
    std::string patientName;
    std::string patientID;
    std::string patientBirthDate;      // DICOM DA：YYYYMMDD
    std::string sex;                   // DICOM CS
    std::string studyUID;              // 引用的 Study Instance UID
    std::string seriesUID;             // 引用的 Series Instance UID
    std::string studyDate;             // DICOM DA
    std::string studyDescription;
    std::string institution;
    std::string refSopClassUID;        // 证据图像的 SOP Class UID（空则按 CT 图像处理）
    std::string refSopUID;             // 证据图像（首个切片）的 SOP Instance UID
    std::string refSeriesUID;          // 证据图像的 Series Instance UID
    /**
     * 证据图像文件绝对路径（CbctVolume::firstSlicePath）。
     * refSopUID 为空时，writeSrFile 会直接从该文件读 (0008,0018)/(0008,0016)，
     * 避免为此再给 :cbctdeal 加一次元数据透传。
     */
    std::string refImagePath;
};

/** 编码三元组（DICOM Code Sequence 的一项）；单独成结构体是为了让头文件不含 DCMTK 类型 */
struct SrCodeTriple {
    std::string codeValue;
    std::string codingScheme;          // DCM / SRT / UCUM / 99CBCTMEASURE
    std::string codeMeaning;

    SrCodeTriple() {}
    SrCodeTriple(const std::string &value, const std::string &scheme, const std::string &meaning)
        : codeValue(value), codingScheme(scheme), codeMeaning(meaning) {}
};

/** 一条 SR 测量内容项（NUM 类型：概念名 + 数值 + 单位） */
struct SrMeasureItem {
    SrCodeTriple concept;              // 概念名（作为 content item 的 Concept Name）
    std::string name;                  // 医生命名（作为 hasConceptMod / 备注展示）
    double value = 0.0;
    std::string unit;                  // PRD 使用的单位符号：mm / deg / cm3 / cm2 / HU
    std::string note;                  // 中文备注，写入 UT 文本
};

/**
 * DICOM SR（Comprehensive SR）生成（PRD 5.5.3）。
 *
 * 依赖说明：本模块自行静态链接 DCMTK 的 dcmsr/dcmdata，因此这里的 DSRDocument
 * 实例与 libcbct_native.so 内的 DCMTK 是两套独立副本；两套之间只通过
 * "内存字节/字符串"交互，不传递任何 DCMTK 对象，故不存在单例冲突。
 * 代价是必须在本 .so 内自行注入数据字典（见 initDictionary）。
 *
 * 编码策略（重要，决定了导出文件能否被第三方 SR 阅读器解读）：
 *   1) 概念名优先使用随本仓库 DCMTK 一起编译进 libdcmsr 的 DICOM 受控词表
 *      （codes/dcm.h，Scheme=DCM）。这些码值/含义已逐个在头文件中核对，
 *      不使用未经验证的 SRT 码（本仓库的 codes/srt.h 仅保留 21 条，
 *      通用量词 "Volume"/"Area" 不在其中，写死一个猜测码值会破坏互操作）。
 *   2) DCM 表中确实没有的通用量词（截面积、ROI 体积、骨宽度）使用
 *      DICOM 允许的私有编码方案 "99CBCTMEASURE"（以 99 开头即为本地方案），
 *      并在 Coding Scheme Identification Sequence 中登记，
 *      因此读端至少能拿到 codeMeaning + 数值 + UCUM 单位，不会误判成标准码。
 *   3) 单位用 UCUM：mm / deg / cm3 是本仓库 UCUM 头里已存在的宏；
 *      cm2 与 [hnsf]（Hounsfield unit）直接用字面码值构造。
 *   4) LO / PN / CS 属性只写 ASCII（中文会被过滤，避免 SpecificCharacterSet
 *      与 VR 冲突）；中文文本一律放在 UT 文本内容项里，文档声明 UTF-8。
 *
 * 全部函数返回 bool（false 时 outError 给出 OFCondition 文本），
 * 因为 dcmsr 的 API 以 OFCondition 报错，而 JNI 层不适合传播 C++ 异常。
 */
namespace SrReport {

    /** 本模块使用的私有编码方案标识（以 "99" 开头 = 本地方案） */
    extern const char *const kLocalScheme;

    /** 注入本 .so 的 DCMTK 数据字典；dictDir 为存放 dicom.dic 的目录 */
    bool initDictionary(const std::string &dictDir, std::string &outError);

    /** PRD 单位符号 -> UCUM 编码三元组（未知单位退化为 {pixels}，保证 NUM 项有合法单位） */
    SrCodeTriple unitCodeFor(const std::string &unit);

    /** 测量类型（MeasureType，M-01 ~ M-08）-> 概念名编码，供上层组装 SrMeasureItem */
    SrCodeTriple conceptFor(int measureType);

    /**
     * 生成 Comprehensive SR 并落盘。
     * @param outPath 目标 .dcm 绝对路径（上层已按 {StudyUID}_{SeriesUID}.sr.dcm 命名）
     * @param outSopUID 非空时回填新建 SR 的 SOP Instance UID（供 C-STORE 归档）
     */
    bool writeSrFile(const std::string &outPath,
                     const SrPatientInfo &info,
                     const std::vector<SrMeasureItem> &items,
                     const std::vector<Implant> &implants,
                     const std::vector<NervePath> &nervePaths,
                     const std::string &operatorName,
                     const std::string &description,
                     std::string &outSopUID,
                     std::string &outError);

    /**
     * 读取已生成的 SR 文件并回传文档类别（自校验用，PRD 验收 AC-08）。
     * 返回 true 表示文件可被 DCMTK 重新解析，outDocType 形如 "Comprehensive"。
     * outSummary 回填 "内容项数量 / 首条内容项" 之类的自检摘要，便于日志核对。
     */
    bool verifySrFile(const std::string &path,
                      std::string &outDocType,
                      std::string &outSummary,
                      std::string &outError);

    /** 新建 SOP Instance UID（DCMTK dcmGenerateUniqueIdentifier 生成） */
    std::string newSopInstanceUid();

}   // namespace SrReport

#endif // DCMTKDEMO_SRREPORT_H
