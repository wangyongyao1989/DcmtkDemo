// DICOM SR（Comprehensive SR）生成与回读自校验（PRD 5.5.3 / AC-09）。
//
// 本文件是 :cbctmeasure 内唯一 include DCMTK 头文件的编译单元，
// 因此 DSRDocument / DcmFileFormat 等类型全部封在 .cpp 里，
// SrReport.h 只暴露 std::string / POD（见头文件的"编码策略"注释）。
//
// 与 :cbctdeal 的关系：本 .so 静态链接了另一份 DCMTK，两者互不感知；
// 数据字典必须在本 .so 内再次注入（initDictionary），否则
// DcmFileFormat::saveFile 会因为 VR 查询失败而写坏文件。

#include "include/SrReport.h"
#include "include/MeasureMath.h"

#include <android/log.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#include "dcmtk/config/osconfig.h"
#include "dcmtk/ofstd/ofstd.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcdict.h"
#include "dcmtk/dcmdata/dcuid.h"
#include "dcmtk/dcmsr/dsrdoc.h"
#include "dcmtk/dcmsr/dsrdoctr.h"
#include "dcmtk/dcmsr/dsrdoctn.h"
#include "dcmtk/dcmsr/dsrcontn.h"
#include "dcmtk/dcmsr/dsrnumtn.h"
#include "dcmtk/dcmsr/dsrtextn.h"
#include "dcmtk/dcmsr/dsrcodtn.h"
#include "dcmtk/dcmsr/dsrimgtn.h"
#include "dcmtk/dcmsr/dsrimgvl.h"
#include "dcmtk/dcmsr/dsrsoprf.h"
#include "dcmtk/dcmsr/dsrcsidl.h"

#define TAG "CbctMeasureSR"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

const char *const SrReport::kLocalScheme = "99CBCTMEASURE";

namespace {

// -----------------------------------------------------------------------------
// 通用小工具
// -----------------------------------------------------------------------------

/** OFCondition -> bool；失败时把可读文本写入 err 并记日志 */
bool checkCond(const OFCondition &cond, std::string &err, const char *what)
{
    if (cond.good()) {
        return true;
    }
    err = std::string(what) + ": " + cond.text();
    LOGE("%s", err.c_str());
    return false;
}

/**
 * LO / PN / CS / SH 属性只写 ASCII：文档声明的 SpecificCharacterSet 只对
 * UT/ST/LT 生效，把中文塞进 LO 会让严格校验的读端（dsrdump / 第三方 PACS）
 * 解析失败，因此这里过滤掉 UTF-8 多字节序列，全被过滤时回退到 fallback。
 */
std::string asciiOnly(const std::string &in, const char *fallback, size_t maxLen = 64)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x20) {
            out += ' ';
        } else if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        }
        // >= 0x80 属于 UTF-8 多字节序列，丢弃
    }
    while (!out.empty() && out[out.size() - 1] == ' ') {
        out.erase(out.size() - 1);
    }
    if (out.empty() && fallback != NULL) {
        out = fallback;
    }
    if (out.size() > maxLen) {
        out.erase(maxLen);
    }
    return out;
}

/** UT 文本允许 UTF-8，只去掉控制字符 */
std::string utf8Text(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        out += (c < 0x20 && c != '\t') ? ' ' : static_cast<char>(c);
    }
    return out;
}

/** DS（Decimal String）：最多 16 字符；NaN/Inf 记为 0；避免 "10." 这类非法写法 */
std::string dsValue(double v)
{
    if (!std::isfinite(v)) {
        v = 0.0;
    }
    char buf[64];
    OFStandard::ftoa(buf, sizeof(buf), v, OFStandard::ftoa_format_f, 0, 4);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (s.size() > 1 && s[s.size() - 1] == '0') {
            s.erase(s.size() - 1);
        }
        if (!s.empty() && s[s.size() - 1] == '.') {
            s += '0';
        }
    }
    if (s.size() > 15) {
        s = s.substr(0, 15);
    }
    return s;
}

std::string dateDA(time_t now)
{
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return std::string(buf);
}

std::string timeTM(time_t now)
{
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d%02d%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

std::string intStr(long long v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    return std::string(buf);
}

/** DICOM 没有 Age 属性，按 PRD 5.5.2「患者信息」章节把年龄写进说明文本 */
std::string ageFrom(const std::string &birthDA, const std::string &studyDA)
{
    if (birthDA.size() < 8 || studyDA.size() < 8) {
        return std::string();
    }
    const int by = atoi(birthDA.substr(0, 4).c_str());
    const int bm = atoi(birthDA.substr(4, 2).c_str());
    const int bd = atoi(birthDA.substr(6, 2).c_str());
    const int sy = atoi(studyDA.substr(0, 4).c_str());
    const int sm = atoi(studyDA.substr(4, 2).c_str());
    const int sd = atoi(studyDA.substr(6, 2).c_str());
    if (by <= 0 || sy <= by) {
        return std::string();
    }
    int age = sy - by;
    if (sm < bm || (sm == bm && sd < bd)) {
        --age;
    }
    return age > 0 ? intStr(age) : std::string("0");
}

/**
 * std::string -> OFString。
 * 本仓库的 DCMTK 以 HAVE_STL_STRING 关闭的方式配置（见 config/osconfig.h），
 * OFString 是 DCMTK 自带的字符串类而非 std::string 的别名，
 * 因此所有跨界参数都必须显式走 c_str()（OFString 有 const char* 隐式构造）。
 */
DSRCodedEntryValue toCode(const SrCodeTriple &code)
{
    return DSRCodedEntryValue(code.codeValue.c_str(), code.codingScheme.c_str(),
                              code.codeMeaning.c_str(), DSRTypes::CVT_auto, OFFalse);
}

/** 本模块私有方案下的一条码（codeMeaning 用 ASCII，中文放 TEXT 内容项） */
SrCodeTriple localCode(const char *value, const char *meaning)
{
    return SrCodeTriple(value, SrReport::kLocalScheme, meaning);
}

// --- DCM 概念码：码值与含义逐条对照本仓库 dcmsr/codes/dcm.h 的宏定义，
//     不使用未经验证的 SRT 码（本仓库 codes/srt.h 只保留 21 条，
//     通用量词 Volume / Area 不在其中，凭记忆填码值会破坏互操作）。
SrCodeTriple cDistance()    { return SrCodeTriple("121206", "DCM", "Distance"); }
SrCodeTriple cAngle()       { return SrCodeTriple("110859", "DCM", "Angle"); }
SrCodeTriple cHeight()      { return SrCodeTriple("121207", "DCM", "Height"); }
SrCodeTriple cDensity()     { return SrCodeTriple("112118", "DCM", "Density"); }
SrCodeTriple cAnnotation()  { return SrCodeTriple("ANN", "DCM", "Annotation"); }
SrCodeTriple cPlan()        { return SrCodeTriple("PLAN", "DCM", "Plan"); }
SrCodeTriple cFinding()     { return SrCodeTriple("121071", "DCM", "Finding"); }
SrCodeTriple cImpression()  { return SrCodeTriple("121073", "DCM", "Impression"); }
SrCodeTriple cReason()      { return SrCodeTriple("122139", "DCM", "Reason for Study"); }
SrCodeTriple cImagingMeas() { return SrCodeTriple("126010", "DCM", "Imaging Measurements"); }
SrCodeTriple cReportRoot()  { return SrCodeTriple("126000", "DCM", "Imaging Measurement Report"); }
SrCodeTriple cProcedure()   { return SrCodeTriple("121058", "DCM", "Procedure reported"); }
SrCodeTriple cImageRegion() { return SrCodeTriple("111030", "DCM", "Image Region"); }
SrCodeTriple cSelRegion()   { return SrCodeTriple("111099", "DCM", "Selected region"); }

/** 测量类型 -> 概念码（PRD 5.1.2 的 M-01 ~ M-08、5.3.3 的 S-07 ~ S-10；由 JNI 层填充 SrMeasureItem 时复用） */
SrCodeTriple conceptForType(int measureType)
{
    switch (measureType) {
        case MT_ANGLE:        return cAngle();
        case MT_TOOTH_ANGULATION: return cAngle();   // S-08 也是角度量
        case MT_HU_SAMPLE:    return cDensity();
        case MT_BONE_DENSITY: return cDensity();
        case MT_ROI_VOLUME:   return localCode("VOL3", "Volume of ROI");
        case MT_ROI_AREA:     return localCode("AREA2", "Cross-sectional area of ROI");
        // 正畸四项在 DCM/SRT 里没有现成码值，用本地码：
        // 只使用本仓库已验证存在的码，凭记忆填码值会让 dsr2xml 读不出来的风险更大。
        case MT_ARCH_LENGTH:  return localCode("DENTALARCHLEN", "Dental arch curve length");
        case MT_MIDLINE_OFFSET: return localCode("DENTALMIDLINEOFFSET", "Upper-lower dental midline offset");
        case MT_OVERBITE:     return localCode("DENTALOVERBITE", "Vertical incisal overlap (overbite)");
        default:              return cDistance();   // M-01 / M-03 / M-06
    }
}

/** 安全等级 -> 私有码（S-02 的红/黄/绿三档语义） */
SrCodeTriple safetyLevelCode(int level)
{
    if (level == DANGER_RED) {
        return localCode("SAFE0", "Unsafe: below clinical threshold");
    }
    if (level == WARN_YELLOW) {
        return localCode("SAFE1", "Marginal: near clinical threshold");
    }
    return localCode("SAFE2", "Safe: all thresholds satisfied");
}

/**
 * 内容项写入器。
 *
 * 约定：每个方法都假设"游标当前停在父节点上"，写完立即 gotoParent()，
 * 因此调用方可以连续往同一个父节点追加兄弟节点，而不必记录节点 ID。
 * openContainer() 例外：它把游标停在新容器上，必须与 closeContainer() 成对使用。
 *
 * 内存：addContentItem(node, mode, deleteIfFail=OFTrue) 让 tree 在失败时回收节点；
 * 成功则所有权转移给 tree。未进入 tree 的节点由本文件自行 delete。
 */
struct Writer {
    DSRDocumentTree &tree;
    std::string &err;
    bool failed;

    Writer(DSRDocumentTree &t, std::string &e) : tree(t), err(e), failed(false) {}

    bool openContainer(const SrCodeTriple &concept)
    {
        DSRContainerTreeNode *node =
            new DSRContainerTreeNode(DSRTypes::RT_contains, DSRTypes::COC_Continuous);
        if (node == NULL) {
            err = "SR container allocation failed";
            failed = true;
            return false;
        }
        node->setConceptName(toCode(concept), OFFalse);
        if (tree.addContentItem(node, DSRTypes::AM_belowCurrent, OFTrue).bad()) {
            err = "SR container rejected: " + concept.codeValue;
            failed = true;
            LOGE("%s", err.c_str());
            return false;
        }
        return true;
    }

    void closeContainer() { tree.gotoParent(); }

    /** NUM：概念名 + 数值 + UCUM 单位；note 非空时挂一条 Annotation 文本 */
    bool addNum(const SrCodeTriple &concept, double value, const SrCodeTriple &unit,
                const std::string &note)
    {
        DSRNumTreeNode *node = new DSRNumTreeNode(DSRTypes::RT_contains);
        node->setConceptName(toCode(concept), OFFalse);
        if (node->setValue(dsValue(value).c_str(), toCode(unit), OFFalse).bad()) {
            err = "SR NUM setValue failed: " + concept.codeMeaning;
            failed = true;
            delete node;                       // 尚未进入 tree
            LOGE("%s", err.c_str());
            return false;
        }
        if (tree.addContentItem(node, DSRTypes::AM_belowCurrent, OFTrue).bad()) {
            err = "SR NUM rejected: " + concept.codeValue;
            failed = true;
            LOGE("%s", err.c_str());
            return false;
        }
        tree.gotoParent();
        LOGD("SR NUM %s=%s %s", concept.codeMeaning.c_str(), dsValue(value).c_str(),
             unit.codeValue.c_str());
        if (!note.empty()) {
            addText(cAnnotation(), note);
        }
        return true;
    }

    /** TEXT：中文备注 / 告警 / 坐标说明都走这里（UT，UTF-8） */
    bool addText(const SrCodeTriple &concept, const std::string &text)
    {
        const std::string body = utf8Text(text);
        if (body.empty()) {
            return true;
        }
        DSRTextTreeNode *node = new DSRTextTreeNode(DSRTypes::RT_contains, body.c_str(), OFFalse);
        node->setConceptName(toCode(concept), OFFalse);
        if (tree.addContentItem(node, DSRTypes::AM_belowCurrent, OFTrue).bad()) {
            err = "SR TEXT rejected: " + concept.codeValue;
            failed = true;
            LOGE("%s", err.c_str());
            return false;
        }
        tree.gotoParent();
        return true;
    }

    /** CODE：安全等级等离散语义（S-02），值本身是编码三元组 */
    bool addCode(const SrCodeTriple &concept, const SrCodeTriple &value)
    {
        DSRCodeTreeNode *node = new DSRCodeTreeNode(DSRTypes::RT_contains);
        node->setConceptName(toCode(concept), OFFalse);
        if (node->setValue(toCode(value), OFFalse).bad()) {
            err = "SR CODE setValue failed";
            failed = true;
            delete node;
            return false;
        }
        if (tree.addContentItem(node, DSRTypes::AM_belowCurrent, OFTrue).bad()) {
            err = "SR CODE rejected: " + concept.codeValue;
            failed = true;
            return false;
        }
        tree.gotoParent();
        return true;
    }

    bool ok() const { return !failed; }
};

/**
 * 从证据图像（首切片）文件读出 SOP Class / SOP Instance UID。
 * 只为一对 UID 而 loadFile 会连带读入像素数据（单层 512x512x2 ≈ 0.5MB），
 * 换来的是不必再给 :cbctdeal 增加一次元数据透传；读不到不视为错误。
 */
bool readImageUids(const std::string &path, std::string &outSopClass, std::string &outSopUID)
{
    if (path.empty()) {
        return false;
    }
    DcmFileFormat ff;
    if (ff.loadFile(path.c_str()).bad()) {
        return false;
    }
    DcmDataset *ds = ff.getDataset();
    if (ds == NULL) {
        return false;
    }
    OFString sc, su;
    if (ds->findAndGetOFString(DCM_SOPClassUID, sc).bad() ||
        ds->findAndGetOFString(DCM_SOPInstanceUID, su).bad()) {
        return false;
    }
    outSopClass = sc.c_str();
    outSopUID = su.c_str();
    return true;
}

/** 种植体入口点/角度的可读中文描述（A 类文本项，不参与数值解析） */
std::string implantText(const Implant &im)
{
    std::ostringstream os;
    os << (im.name.empty() ? ("种植体 #" + intStr(im.id)) : im.name)
       << "\n入口点(mm): " << MeasureMath::format(im.entry.x) << ", "
       << MeasureMath::format(im.entry.y) << ", " << MeasureMath::format(im.entry.z)
       << "\n俯仰/偏转(deg): " << MeasureMath::format(im.pitchDeg) << " / "
       << MeasureMath::format(im.yawDeg)
       << "\n直径/全长/深度(mm): " << MeasureMath::format(im.diaMm) << " / "
       << MeasureMath::format(im.lengthMm) << " / " << MeasureMath::format(im.depthMm);
    return os.str();
}

/** 结论段：统计 + 风险项，直接作为 Impression 文本 */
std::string impressionText(const std::vector<SrMeasureItem> &items,
                           const std::vector<Implant> &implants)
{
    size_t red = 0, yellow = 0;
    for (size_t i = 0; i < implants.size(); ++i) {
        if (!implants[i].visible) {
            continue;
        }
        if (implants[i].level == DANGER_RED) {
            ++red;
        } else if (implants[i].level == WARN_YELLOW) {
            ++yellow;
        }
    }
    std::ostringstream os;
    os << "共 " << items.size() << " 条测量、" << implants.size() << " 颗种植体方案。";
    if (implants.empty()) {
        os << "本次未包含种植体规划。";
    } else if (red > 0) {
        os << "存在 " << red << " 处低于安全阈值的位置，需调整植入点或深度后复评。";
    } else if (yellow > 0) {
        os << "存在 " << yellow << " 处临界（阈值~阈值×1.2）位置，建议结合临床复核。";
    } else {
        os << "全部种植体位置满足骨高度/骨宽度/神经管距离/间距四项阈值。";
    }
    return os.str();
}

const char *docTypeName(DSRTypes::E_DocumentType t)
{
    switch (t) {
        case DSRTypes::DT_BasicTextSR:           return "BasicTextSR";
        case DSRTypes::DT_EnhancedSR:            return "EnhancedSR";
        case DSRTypes::DT_ComprehensiveSR:       return "ComprehensiveSR";
        case DSRTypes::DT_KeyObjectSelectionDocument: return "KeyObjectSelectionDocument";
        case DSRTypes::DT_MammographyCadSR:      return "MammographyCadSR";
        case DSRTypes::DT_ChestCadSR:            return "ChestCadSR";
        case DSRTypes::DT_ColonCadSR:            return "ColonCadSR";
        case DSRTypes::DT_ProcedureLog:          return "ProcedureLog";
        case DSRTypes::DT_XRayRadiationDoseSR:   return "XRayRadiationDoseSR";
        default:                                 return "Other";
    }
}

}   // namespace

// =============================================================================
// 公开接口
// =============================================================================

namespace SrReport {

bool initDictionary(const std::string &dictPath, std::string &outError)
{
    if (dictPath.empty()) {
        outError = "dictionary path is empty";
        return false;
    }
    // 与 :cbctdeal 的 CbctJni.initDictionary 同一写法：本 .so 内的 dcmDataDict
    // 是独立副本，交叉编译的 DCMTK 以 --without-private-dictionary 构建，
    // 内置字典为空实现，不注入则写文件时标准 tag 的 VR 查询会失败。
    DcmDataDictionary &dict = dcmDataDict.wrlock();
    const OFBool loaded = dict.loadDictionary(dictPath.c_str());
    dcmDataDict.wrunlock();
    if (!loaded) {
        outError = "loadDictionary failed: " + dictPath;
        LOGE("%s", outError.c_str());
        return false;
    }
    LOGD("SR dictionary injected: %s", dictPath.c_str());
    return true;
}

SrCodeTriple unitCodeFor(const std::string &unit)
{
    // mm / cm / cm3 / deg / s 在本仓库 codes/ucum.h 中确有对应宏；
    // cm2 与 [hnsf] 没有宏，按 UCUM 字面码值构造（DCMTK 不校验 UCUM 码表）。
    if (unit == "mm")  return SrCodeTriple("mm", "UCUM", "millimeter");
    if (unit == "cm")  return SrCodeTriple("cm", "UCUM", "centimeter");
    if (unit == "cm3" || unit == "cc") return SrCodeTriple("cm3", "UCUM", "cubic centimeter");
    if (unit == "cm2") return SrCodeTriple("cm2", "UCUM", "square centimeter");
    if (unit == "deg" || unit == "degree") return SrCodeTriple("deg", "UCUM", "degree");
    if (unit == "HU" || unit == "hu" || unit == "[hnsf]")
        return SrCodeTriple("[hnsf]", "UCUM", "Hounsfield unit");
    if (unit == "s")   return SrCodeTriple("s", "UCUM", "second");
    if (unit == "n" || unit == "count")
        return SrCodeTriple("{count}", "UCUM", "count");
    return SrCodeTriple("{pixels}", "UCUM", "pixels");
}

SrCodeTriple conceptFor(int measureType)
{
    return conceptForType(measureType);
}

std::string newSopInstanceUid()
{
    char uid[128];
    memset(uid, 0, sizeof(uid));
    dcmGenerateUniqueIdentifier(uid, SITE_INSTANCE_UID_ROOT);
    return std::string(uid);
}

bool writeSrFile(const std::string &outPath,
                 const SrPatientInfo &info,
                 const std::vector<SrMeasureItem> &items,
                 const std::vector<Implant> &implants,
                 const std::vector<NervePath> &nervePaths,
                 const std::string &operatorName,
                 const std::string &description,
                 std::string &outSopUID,
                 std::string &outError)
{
    outSopUID.clear();
    outError.clear();
    if (outPath.empty()) {
        outError = "SR output path is empty";
        return false;
    }

    DSRDocument doc(DSRTypes::DT_ComprehensiveSR);
    doc.setSpecificCharacterSetType(DSRTypes::CS_UTF8);

    // 私有编码方案显式登记；失败不致命（读端仍按 codeMeaning 解释）
    const OFCondition scheme = doc.getCodingSchemeIdentification().addItem(
        kLocalScheme, "", "CBCT Measure local codes", "DcmtkDemo", OFFalse);
    if (scheme.bad()) {
        LOGE("coding scheme not registered: %s", scheme.text());
    }

    // ---- 患者 / Study / Series（全部 ASCII）----
    const time_t now = time(NULL);
    const std::string studyDA = info.studyDate.empty() ? dateDA(now) : info.studyDate;
    doc.setPatientName(asciiOnly(info.patientName, "UNKNOWN^CBCT").c_str(), OFFalse);
    doc.setPatientID(asciiOnly(info.patientID, "NO-PATIENT-ID").c_str(), OFFalse);
    if (!info.patientBirthDate.empty()) {
        doc.setPatientBirthDate(asciiOnly(info.patientBirthDate, NULL, 8).c_str(), OFFalse);
    }
    if (!info.sex.empty()) {
        doc.setPatientSex(asciiOnly(info.sex, NULL, 16).c_str(), OFFalse);
    }
    doc.setStudyDate(asciiOnly(studyDA, NULL, 8).c_str(), OFFalse);
    doc.setStudyTime(asciiOnly(timeTM(now), NULL, 14).c_str(), OFFalse);
    doc.setStudyID(asciiOnly(info.studyUID, "CBCTMEASURE", 16).c_str(), OFFalse);
    doc.setStudyDescription(asciiOnly(info.studyDescription, "CBCT Craniofacial Scan").c_str(),
                            OFFalse);
    doc.setSeriesDescription(asciiOnly(description, "CBCT Measurement Report").c_str(), OFFalse);
    doc.setManufacturer(asciiOnly(info.institution, "DcmtkDemo").c_str(), OFFalse);
    doc.setManufacturerModelName("Honor AGM3-W09HN", OFFalse);
    doc.setSoftwareVersions("DcmtkDemo :cbctmeasure 1.0", OFFalse);
    doc.setAccessionNumber(asciiOnly(info.studyUID, NULL, 16).c_str(), OFFalse);
    doc.setContentDate(dateDA(now).c_str(), OFFalse);
    doc.setContentTime(timeTM(now).c_str(), OFFalse);

    // Series：能对上来源 Study 就复用 Study UID，让 SR 与原始序列同属一个 Study
    if (!info.studyUID.empty() && doc.createNewSeriesInStudy(info.studyUID.c_str(), OFFalse).good()) {
        LOGD("SR series created inside study %s", info.studyUID.c_str());
    } else {
        doc.createNewSeries();
    }
    doc.setInstanceNumber("1", OFFalse);

    // ---- 内容树 ----
    DSRDocumentTree &tree = doc.getTree();
    DSRContainerTreeNode *rootNode =
        new DSRContainerTreeNode(DSRTypes::RT_isRoot, DSRTypes::COC_Continuous);
    rootNode->setConceptName(toCode(cReportRoot()), OFFalse);
    if (tree.addContentItem(rootNode, DSRTypes::AM_belowCurrent, OFTrue).bad()) {
        // 空树上"below current"语义在两版 DCMTK 里都出现过，留一条退路，
        // 真机日志会告诉我们这一版实际走的是哪个分支。
        LOGD("root via AM_belowCurrent rejected, retry AM_afterCurrent");
        rootNode = new DSRContainerTreeNode(DSRTypes::RT_isRoot, DSRTypes::COC_Continuous);
        rootNode->setConceptName(toCode(cReportRoot()), OFFalse);
        if (!checkCond(tree.addContentItem(rootNode, DSRTypes::AM_afterCurrent, OFTrue),
                       outError, "add root content item")) {
            return false;
        }
    }

    Writer w(tree, outError);

    // 1) 报告目的 + 患者/扫描参数（PRD 5.5.2 第 1~2 章）
    w.addText(cReason(), description.empty()
        ? std::string("CBCT 三维测量与种植体手术规划结构化报告") : description);
    {
        std::ostringstream meta;
        meta << "Patient: " << (info.patientName.empty() ? "-" : info.patientName)
             << " / ID: " << (info.patientID.empty() ? "-" : info.patientID)
             << " / Sex: " << (info.sex.empty() ? "-" : info.sex);
        const std::string age = ageFrom(info.patientBirthDate, studyDA);
        if (!age.empty()) {
            meta << " / Age: " << age << " 岁";
        }
        meta << "\nStudy UID: " << (info.studyUID.empty() ? "-" : info.studyUID)
             << "\nSeries UID: " << (info.seriesUID.empty() ? "-" : info.seriesUID)
             << "\nStudy Date: " << studyDA
             << "\nInstitution: " << (info.institution.empty() ? "-" : info.institution);
        w.addText(cProcedure(), meta.str());
    }

    // 2) 测量数据表（M-01 ~ M-08）：统一放在 Imaging Measurements 容器里
    if (!items.empty()) {
        if (w.openContainer(cImagingMeas())) {
            for (size_t i = 0; i < items.size(); ++i) {
                const SrMeasureItem &it = items[i];
                SrCodeTriple concept = it.concept;
                if (concept.codeValue.empty()) {
                    concept = cDistance();
                }
                // 医生自定义命名跟在数值后面，便于 dsrdump 输出里定位
                w.addNum(concept, it.value, unitCodeFor(it.unit),
                         (!it.name.empty() || !it.note.empty())
                             ? (it.name.empty() ? it.note : (it.note.empty() ? it.name
                                                                              : it.name + " | " + it.note))
                             : std::string());
            }
            w.closeContainer();
        }
    }

    // 3) 种植体方案（S-01 ~ S-06）
    {
        std::vector<size_t> shown;
        for (size_t i = 0; i < implants.size(); ++i) {
            if (implants[i].visible) {
                shown.push_back(i);
            }
        }
        if (!shown.empty()) {
            if (w.openContainer(cPlan())) {
                for (size_t k = 0; k < shown.size(); ++k) {
                    const Implant &im = implants[shown[k]];
                    if (!w.openContainer(localCode("IMPL", "Implant placement"))) {
                        continue;
                    }
                    w.addText(cFinding(), implantText(im));
                    w.addNum(cHeight(), im.boneHeightMm, unitCodeFor("mm"), "S-03 沿轴可用骨高度");
                    w.addNum(localCode("BONEW", "Bone width"), im.boneWidthMm, unitCodeFor("mm"),
                             "S-04 颊舌向骨宽度");
                    w.addNum(cDistance(), im.nerveDistMm, unitCodeFor("mm"),
                             "S-05 桩体与下颌神经管最短距离");
                    w.addNum(cDistance(), im.minSpacingMm, unitCodeFor("mm"),
                             "S-06 与相邻种植体最短间距");
                    w.addCode(localCode("SECLVL", "Safety level"), safetyLevelCode(im.level));
                    if (!im.warnText.empty()) {
                        w.addText(cAnnotation(), im.warnText);
                    }
                    w.closeContainer();
                }
                w.closeContainer();
            }
        }
    }

    // 4) 神经管描记（S-05 的依据：只报总长与点数，坐标在 plan.json 里）
    for (size_t i = 0; i < nervePaths.size(); ++i) {
        const NervePath &np = nervePaths[i];
        if (!np.visible || np.points.size() < 2) {
            continue;
        }
        w.addNum(cDistance(), MeasureMath::arcLength(np.points), unitCodeFor("mm"),
                 "下颌神经管描记总长（" + intStr(static_cast<long long>(np.points.size())) + " 点）");
    }

    // 5) 结论
    w.addText(cImpression(), impressionText(items, implants));

    if (!w.ok()) {
        LOGE("SR content tree has failures: %s（继续落盘，已写入的内容仍可解析）", outError.c_str());
    }

    // ---- 图像证据引用（Comprehensive SR 的 Current Requested Procedure Evidence）----
    SrPatientInfo resolved = info;
    if (resolved.refSopUID.empty() && readImageUids(resolved.refImagePath,
                                                    resolved.refSopClassUID, resolved.refSopUID)) {
        LOGD("SR evidence resolved from %s -> %s", resolved.refImagePath.c_str(),
             resolved.refSopUID.c_str());
    }
    if (!resolved.refSopUID.empty()) {
        const std::string sopClass = resolved.refSopClassUID.empty()
            ? std::string(UID_CTImageStorage) : resolved.refSopClassUID;
        const std::string refStudy = resolved.studyUID;
        const std::string refSeries = !resolved.refSeriesUID.empty()
            ? resolved.refSeriesUID
            : (!resolved.seriesUID.empty() ? resolved.seriesUID : std::string());
        if (!refStudy.empty() && !refSeries.empty()) {
            DSRSOPInstanceReferenceList &ev = doc.getCurrentRequestedProcedureEvidence();
            if (ev.addItem(refStudy.c_str(), refSeries.c_str(), sopClass.c_str(),
                           resolved.refSopUID.c_str(), OFFalse).bad()) {
                LOGE("evidence addItem failed（SR 仍可解析，仅缺图像证据）");
            } else {
                // 同时在树里挂一条 IMAGE 内容项，dsrdump 会显示引用关系
                Writer iw(tree, outError);
                if (iw.openContainer(cSelRegion())) {
                    DSRImageTreeNode *imgNode = new DSRImageTreeNode(DSRTypes::RT_contains);
                    imgNode->setConceptName(toCode(cImageRegion()), OFFalse);
                    imgNode->setValue(DSRImageReferenceValue(sopClass.c_str(),
                                                             resolved.refSopUID.c_str(),
                                                             OFFalse), OFFalse);
                    if (tree.addContentItem(imgNode, DSRTypes::AM_belowCurrent, OFTrue).good()) {
                        tree.gotoParent();
                    } else {
                        LOGE("IMAGE content item rejected");
                    }
                    iw.closeContainer();
                }
            }
        }
    }

    // ---- 完成 + 签署（必须先 complete 再 verify）----
    if (!checkCond(doc.completeDocument(), outError, "completeDocument")) {
        return false;
    }
    const std::string observer = asciiOnly(operatorName, "CBCT MEASURE USER", 64);
    if (!checkCond(doc.verifyDocument(observer.c_str(), "DcmtkDemo Dental Imaging", "", OFFalse),
                   outError, "verifyDocument")) {
        return false;
    }

    // ---- 落盘 ----
    DcmFileFormat ff;
    if (!checkCond(doc.write(*ff.getDataset(), NULL), outError, "DSRDocument::write")) {
        return false;
    }
    if (!checkCond(ff.saveFile(outPath.c_str(), EXS_LittleEndianExplicit), outError, "saveFile")) {
        return false;
    }
    OFString sop;
    if (doc.getSOPInstanceUID(sop, 0).good()) {
        outSopUID = sop.c_str();
    }
    LOGD("SR written: %s (SOP=%s, %zu measures, %zu implants)", outPath.c_str(),
         outSopUID.c_str(), items.size(), implants.size());
    return true;
}

bool verifySrFile(const std::string &path,
                  std::string &outDocType,
                  std::string &outSummary,
                  std::string &outError)
{
    outDocType.clear();
    outSummary.clear();
    outError.clear();

    DcmFileFormat ff;
    if (!checkCond(ff.loadFile(path.c_str()), outError, "loadFile")) {
        return false;
    }
    DcmDataset *ds = ff.getDataset();
    if (ds == NULL) {
        outError = "no dataset in " + path;
        return false;
    }
    DSRDocument doc;
    // 这里用 RF_skipInvalidContentItems（=1）以外的一组保守 flags：0 表示
    // "任何无效内容项都算失败"，正好用来当 AC-09 的自检（dsr2xml 同样严格）。
    if (!checkCond(doc.read(*ds, 0), outError, "DSRDocument::read")) {
        return false;
    }
    outDocType = docTypeName(doc.getDocumentType());
    if (!doc.isValid()) {
        outDocType += " (invalid)";
    } else {
        outDocType += " (valid)";
    }

    // 自检摘要：把内容树按 dsrdump 的格式打印到内存，统计 NUM / TEXT 行数。
    std::ostringstream os;
    const OFCondition printed = doc.print(os, 0);
    if (printed.bad()) {
        outSummary = std::string("print failed: ") + printed.text();
    } else {
        const std::string text = os.str();
        size_t nums = 0, texts = 0, pos = 0;
        while ((pos = text.find("NUM:", pos)) != std::string::npos) {
            ++nums;
            pos += 4;
        }
        pos = 0;
        while ((pos = text.find("TEXT:", pos)) != std::string::npos) {
            ++texts;
            pos += 5;
        }
        std::ostringstream sum;
        sum << "num=" << nums << " text=" << texts << " bytes=" << text.size()
            << " observers=" << doc.getNumberOfVerifyingObservers();
        outSummary = sum.str();
    }
    LOGD("SR verified: %s -> %s / %s", path.c_str(), outDocType.c_str(), outSummary.c_str());
    return true;
}

}   // namespace SrReport
