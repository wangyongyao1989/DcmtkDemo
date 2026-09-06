#include "CbctSeriesParser.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include <android/log.h>

#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcrledrg.h"
#include "dcmtk/dcmjpeg/djdecode.h"
#include "dcmtk/dcmjpls/djdecode.h"

#define TAG "CbctNative"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

/** 单调时钟计时（多线程下 clock() 统计的是 CPU 时间会虚高） */
long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// 解码器只注册一次（JPEG / JPEG-LS / RLE，覆盖常见压缩格式）
void ensureCodecsRegistered() {
    static bool registered = false;
    if (!registered) {
        DJDecoderRegistration::registerCodecs();
        DJLSDecoderRegistration::registerCodecs();
        DcmRLEDecoderRegistration::registerCodecs();
        registered = true;
        LOGD("JPEG / JPEG-LS / RLE decoders registered");
    }
}

/** Pass A 阶段读取到的切片元数据（不含像素，内存开销极小） */
struct SliceMeta {
    std::string path;
    double z = 0.0;                  // ImagePositionPatient[2]，排序唯一依据
    bool hasZ = false;
    double sliceLocation = 0.0;     // 兜底排序依据
    bool hasSliceLocation = false;
    long instanceNumber = 0;         // 二级兜底排序依据
    bool hasInstance = false;

    double thickness = 1.0;
    double spacingX = 1.0, spacingY = 1.0;
    double slope = 1.0, intercept = 0.0;
    int rows = 0, cols = 0;
    int pixelRepresentation = 0;
    int bitsAllocated = 16;
    unsigned long frames = 1;                       // 多帧文件的帧数
    std::vector<double> frameOffsets;               // 多帧：每帧相对首帧 Z 偏移

    // 患者与检查信息（取第一个有效切片）
    std::string patientName, patientID, patientSex, patientBirthDate;
    std::string studyDate, modality, manufacturer;
    double windowWidth = 0.0, windowCenter = 0.0;
    bool hasWindow = false;
};

/** 枚举目录下的普通文件（不递归），按文件名稳定排序仅用于进度展示 */
bool listFiles(const std::string &dir, std::vector<std::string> &out) {
    DIR *d = opendir(dir.c_str());
    if (!d) {
        LOGE("opendir failed: %s", dir.c_str());
        return false;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_type != DT_REG) continue;   // 跳过子目录
        std::string name = ent->d_name;
        if (name.size() > 0 && name[0] == '.') continue;
        out.push_back(dir + "/" + name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return true;
}

/**
 * 读取单个 DICOM 文件的元数据（Pass A）。
 * 说明：loadFile 会连 PixelData 一起读入，但函数返回后 DcmFileFormat 析构即释放，
 * 单文件级别的内存峰值很小，且多线程下每个线程持有独立的 DcmFileFormat 对象。
 */
bool readMeta(const std::string &path, SliceMeta &m) {
    m.path = path;
    DcmFileFormat ff;
    if (ff.loadFile(path.c_str()).bad()) {
        return false;   // 非 DICOM 或损坏文件，直接过滤
    }
    DcmDataset *ds = ff.getDataset();

    Uint16 rows = 0, cols = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, cols);
    if (rows == 0 || cols == 0) return false;   // 无有效图像
    m.rows = rows;
    m.cols = cols;

    Float64 v;
    // PixelSpacing [行间距, 列间距]（DS 多值，pos0=row/纵向，pos1=col/横向）
    if (ds->findAndGetFloat64(DCM_PixelSpacing, v, 0).good() && v > 0) m.spacingY = v;
    if (ds->findAndGetFloat64(DCM_PixelSpacing, v, 1).good() && v > 0) m.spacingX = v;
    if (ds->findAndGetFloat64(DCM_SliceThickness, v).good() && v > 0) m.thickness = v;

    // HU 转换参数
    if (ds->findAndGetFloat64(DCM_RescaleSlope, v).good()) m.slope = v;
    if (ds->findAndGetFloat64(DCM_RescaleIntercept, v).good()) m.intercept = v;

    Uint16 pr = 0, bits = 16;
    ds->findAndGetUint16(DCM_PixelRepresentation, pr);
    ds->findAndGetUint16(DCM_BitsAllocated, bits);
    m.pixelRepresentation = pr;
    m.bitsAllocated = (bits >= 8) ? bits : 16;

    // 切片定位：ImagePositionPatient[2] 是 Z 轴排序唯一可靠依据
    OFString s;
    if (ds->findAndGetOFString(DCM_ImagePositionPatient, s, 2).good() && !s.empty()) {
        m.z = atof(s.c_str());
        m.hasZ = true;
    } else if (ds->findAndGetFloat64(DCM_SliceLocation, v).good()) {
        m.sliceLocation = v;
        m.hasSliceLocation = true;
    }
    Sint32 inst = 0;
    if (ds->findAndGetSint32(DCM_InstanceNumber, inst).good()) {
        m.instanceNumber = inst;
        m.hasInstance = true;
    }

    // 多帧封装适配：NumberOfFrames + GridFrameOffsetVector
    Sint32 nf = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, nf).good() && nf > 1) {
        m.frames = (unsigned long) nf;
        m.frameOffsets.reserve(m.frames);
        for (unsigned long k = 0; k < m.frames; ++k) {
            Float64 off;
            if (ds->findAndGetFloat64(DCM_GridFrameOffsetVector, off, k).good()) {
                m.frameOffsets.push_back(off);
            } else {
                m.frameOffsets.push_back(k * m.thickness);   // 缺失时按层厚估算
            }
        }
    }

    // 患者与检查信息
    if (ds->findAndGetOFString(DCM_PatientName, s).good()) m.patientName = s.c_str();
    if (ds->findAndGetOFString(DCM_PatientID, s).good()) m.patientID = s.c_str();
    if (ds->findAndGetOFString(DCM_PatientSex, s).good()) m.patientSex = s.c_str();
    if (ds->findAndGetOFString(DCM_PatientBirthDate, s).good()) m.patientBirthDate = s.c_str();
    if (ds->findAndGetOFString(DCM_StudyDate, s).good()) m.studyDate = s.c_str();
    if (ds->findAndGetOFString(DCM_Modality, s).good()) m.modality = s.c_str();
    if (ds->findAndGetOFString(DCM_Manufacturer, s).good()) m.manufacturer = s.c_str();

    Float64 ww = 0, wc = 0;
    if (ds->findAndGetFloat64(DCM_WindowWidth, ww).good() && ww >= 1.0 &&
        ds->findAndGetFloat64(DCM_WindowCenter, wc).good()) {
        m.windowWidth = ww;
        m.windowCenter = wc;
        m.hasWindow = true;
    }
    return true;
}

/**
 * 读取单个 DICOM 文件的 16bit 像素（Pass B）。
 * 压缩格式（JPEG / JPEG-LS / RLE）通过 chooseRepresentation 解压到显式小端后读取。
 */
bool readPixels(const std::string &path, int bitsAllocated, size_t expectedWords,
                std::vector<Uint16> &out) {
    DcmFileFormat ff;
    if (ff.loadFile(path.c_str()).bad()) {
        LOGE("readPixels: loadFile failed: %s", path.c_str());
        return false;
    }
    DcmDataset *ds = ff.getDataset();

    // 统一转换为显式小端（处理压缩格式、大端格式及隐式格式）
    // 如果是未压缩格式，此操作在 DCMTK 中非常快
    ds->chooseRepresentation(EXS_LittleEndianExplicit, nullptr);

    out.assign(expectedWords, 0);

    // 1. 尝试直接以 16-bit 方式获取 (OW 类型)
    const Uint16 *p16 = nullptr;
    unsigned long count = 0;
    if (ds->findAndGetUint16Array(DCM_PixelData, p16, &count).good() && p16) {
        size_t n = std::min((size_t)count, expectedWords);
        memcpy(out.data(), p16, n * 2);
        return true;
    }

    // 2. 尝试以 8-bit 方式获取 (OB 类型)
    const Uint8 *p8 = nullptr;
    if (ds->findAndGetUint8Array(DCM_PixelData, p8, &count).good() && p8) {
        if (bitsAllocated <= 8) {
            size_t n = std::min((size_t)count, expectedWords);
            for (size_t i = 0; i < n; ++i) out[i] = p8[i];
        } else {
            // 16-bit 数据被存为了 OB 类型
            size_t n = std::min((size_t)count / 2, expectedWords);
            memcpy(out.data(), p8, n * 2);
        }
        return true;
    }

    LOGE("readPixels: all access methods failed for %s (xfer=%s)",
         path.c_str(), DcmXfer(ds->getOriginalXfer()).getXferName());
    return false;
}

/** 动态任务分片的多线程 for（原子计数取任务，天然负载均衡） */
void parallelFor(size_t total, const std::function<void(size_t)> &body) {
    if (total == 0) return;
    unsigned hw = std::thread::hardware_concurrency();
    unsigned n = (hw == 0) ? 4u : hw;
    if (n > 8u) n = 8u;                       // 线程数 = min(CPU核数, 8)
    if (n <= 1 || total < 8) {
        for (size_t i = 0; i < total; ++i) body(i);
        return;
    }
    std::vector<std::thread> pool;
    std::atomic<size_t> next(0);
    for (unsigned t = 0; t < n; ++t) {
        pool.emplace_back([&] {
            size_t i;
            while ((i = next.fetch_add(1)) < total) body(i);
        });
    }
    for (auto &th : pool) th.join();
}

} // namespace

CbctVolume::~CbctVolume() {
    delete[] data;
    data = nullptr;
}

CbctVolume *CbctSeriesParser::loadSeries(const std::string &dir,
                                         const ProgressFn &progress,
                                         std::string &err) {
    if (dir.empty()) {
        err = "empty dir";
        return nullptr;
    }
    ensureCodecsRegistered();
    const long long t0 = nowMs();

    // ---------- 1. 枚举文件 ----------
    std::vector<std::string> files;
    if (!listFiles(dir, files) || files.empty()) {
        err = "no files in dir: " + dir;
        return nullptr;
    }
    LOGD("loadSeries: %zu candidate files in %s", files.size(), dir.c_str());

    // ---------- 2. Pass A：多线程读取元数据 ----------
    std::vector<SliceMeta> metas(files.size());
    std::vector<char> valid(files.size(), 0);
    std::atomic<size_t> metaDone(0);
    parallelFor(files.size(), [&](size_t i) {
        if (readMeta(files[i], metas[i])) valid[i] = 1;
        size_t done = metaDone.fetch_add(1) + 1;
        if (progress) progress(done, files.size() * 2);   // 两阶段共 2N 步
    });

    std::vector<SliceMeta> slices;
    slices.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        if (valid[i]) slices.push_back(metas[i]);
    }
    if (slices.empty()) {
        err = "no valid DICOM file found";
        return nullptr;
    }
    const int skippedFiles = (int) (files.size() - slices.size());
    LOGD("loadSeries: %zu valid, %d skipped", slices.size(), skippedFiles);

    // ---------- 3. 切片排序：Z 坐标优先，文件名排序是被禁止的 ----------
    size_t withZ = 0;
    for (const auto &s : slices) if (s.hasZ) ++withZ;
    if (withZ == slices.size()) {
        std::stable_sort(slices.begin(), slices.end(),
                         [](const SliceMeta &a, const SliceMeta &b) { return a.z < b.z; });
    } else if (withZ == 0) {
        // 无定位坐标：按 SliceLocation，再按 InstanceNumber 兜底
        bool allLoc = true;
        for (const auto &s : slices) if (!s.hasSliceLocation) { allLoc = false; break; }
        if (allLoc) {
            std::stable_sort(slices.begin(), slices.end(),
                             [](const SliceMeta &a, const SliceMeta &b) {
                                 return a.sliceLocation < b.sliceLocation;
                             });
        } else {
            std::stable_sort(slices.begin(), slices.end(),
                             [](const SliceMeta &a, const SliceMeta &b) {
                                 return a.instanceNumber < b.instanceNumber;
                             });
        }
    } else {
        err = "inconsistent positioning tags (mixed ImagePositionPatient)";
        return nullptr;
    }

    // ---------- 4. 统一几何参数（以第一个有效切片为基准，尺寸不一致的剔除） ----------
    const int rows = slices[0].rows;
    const int cols = slices[0].cols;
    const size_t sliceSize = (size_t) rows * cols;
    size_t removed = 0;
    slices.erase(std::remove_if(slices.begin(), slices.end(),
                                [&](const SliceMeta &s) {
                                    if (s.rows == rows && s.cols == cols) return false;
                                    ++removed;
                                    return true;
                                }),
                 slices.end());
    if (slices.empty()) {
        err = "no slice with consistent geometry";
        return nullptr;
    }
    LOGD("loadSeries: geometry %dx%d, %zu inconsistent slices removed", rows, cols, removed);

    // ---------- 5. 计算 Z 间距（取相邻 Z 差的中位数，回退层厚） ----------
    double spacingZ = slices[0].thickness;
    if (withZ == slices.size() && slices.size() >= 2) {
        std::vector<double> diffs;
        diffs.reserve(slices.size() - 1);
        for (size_t i = 1; i < slices.size(); ++i) {
            double dz = slices[i].z - slices[i - 1].z;
            if (dz > 1e-6) diffs.push_back(dz);
        }
        if (!diffs.empty()) {
            std::sort(diffs.begin(), diffs.end());
            spacingZ = diffs[diffs.size() / 2];
        }
    }
    if (spacingZ <= 1e-6) spacingZ = 1.0;

    // ---------- 6. Z 坐标 -> Volume 层索引映射（含断层间隙补全） ----------
    // 对每个切片（及其多帧的每一帧）计算目标 z 索引；间隙处索引跳空，
    // Volume 初始化为 0（HU=intercept，即空气），即自动补全的空白切片。
    const double z0 = slices[0].z;
    struct Target {
        size_t sliceIdx;     // 属于 slices 的下标
        unsigned long frame; // 帧号（单帧文件为 0）
        size_t zIndex;       // Volume 中的层索引
    };
    std::vector<Target> targets;
    targets.reserve(slices.size() * 4);
    for (size_t i = 0; i < slices.size(); ++i) {
        const SliceMeta &sm = slices[i];
        for (unsigned long k = 0; k < sm.frames; ++k) {
            double zf = sm.z;
            if (sm.frames > 1 && k < sm.frameOffsets.size()) zf += sm.frameOffsets[k];
            else if (sm.frames > 1) zf += k * spacingZ;
            long idx = lround((zf - z0) / spacingZ);
            if (idx < 0) idx = 0;
            if (idx > 8192) continue;   // 异常坐标保护
            targets.push_back({i, k, (size_t) idx});
        }
    }
    if (targets.empty()) {
        err = "no target slices";
        return nullptr;
    }
    // 去重：同一 zIndex 只保留一个来源（后者覆盖前者：重复切片直接跳过）
    std::sort(targets.begin(), targets.end(),
              [](const Target &a, const Target &b) { return a.zIndex < b.zIndex; });
    targets.erase(std::unique(targets.begin(), targets.end(),
                              [](const Target &a, const Target &b) {
                                  return a.zIndex == b.zIndex;
                              }),
                  targets.end());
    const int depth = (int) (targets.back().zIndex + 1);

    // ---------- 7. 分配 Native 堆连续 Volume（内存守卫） ----------
    const double bytes = (double) sliceSize * depth * 2.0;
    if (bytes > 600.0 * 1024.0 * 1024.0) {
        err = "volume too large (" + std::to_string((long long) (bytes / 1048576.0)) + " MB)";
        return nullptr;
    }
    CbctVolume *vol = nullptr;
    try {
        vol = new CbctVolume();
        vol->data = new Uint16[sliceSize * depth]();   // 零初始化 = 空气填充
    } catch (const std::bad_alloc &) {
        delete vol;
        err = "native memory exhausted when allocating volume";
        return nullptr;
    }
    vol->sliceSize = sliceSize;
    vol->width = cols;
    vol->height = rows;
    vol->depth = depth;
    vol->spacingX = slices[0].spacingX;
    vol->spacingY = slices[0].spacingY;
    vol->spacingZ = spacingZ;
    vol->slope = slices[0].slope;
    vol->intercept = slices[0].intercept;
    vol->pixelRepresentation = slices[0].pixelRepresentation;
    vol->sliceCount = (int) targets.size();
    vol->skippedFiles = skippedFiles;
    vol->zMin = z0;
    vol->zMax = z0 + spacingZ * (depth - 1);
    vol->patientName = slices[0].patientName;
    vol->patientID = slices[0].patientID;
    vol->patientSex = slices[0].patientSex;
    vol->patientBirthDate = slices[0].patientBirthDate;
    vol->studyDate = slices[0].studyDate;
    vol->modality = slices[0].modality;
    vol->manufacturer = slices[0].manufacturer;
    if (slices[0].hasWindow) {
        vol->windowWidth = slices[0].windowWidth;
        vol->windowCenter = slices[0].windowCenter;
    } else {
        // CBCT 容错：缺失窗宽窗位时补充默认骨骼窗
        vol->windowWidth = 4000.0;
        vol->windowCenter = 600.0;
    }
    LOGD("loadSeries: volume %dx%dx%d, spacing %.4f/%.4f/%.4f mm, %.1f MB",
         cols, rows, depth, vol->spacingX, vol->spacingY, vol->spacingZ, bytes / 1048576.0);

    // ---------- 8. Pass B：多线程读取像素写入 Volume ----------
    // 按文件分组处理：每个文件（含多帧）只读取/解压一次，
    // 不同文件写入不同 zIndex 区域，线程间无共享写冲突。
    std::vector<std::vector<size_t>> fileTargets(slices.size());
    for (size_t t = 0; t < targets.size(); ++t) {
        fileTargets[targets[t].sliceIdx].push_back(t);
    }
    std::atomic<size_t> pixelDone(0);
    std::atomic<int> loadFailed(0);
    parallelFor(slices.size(), [&](size_t f) {
        if (fileTargets[f].empty()) return;
        const SliceMeta &sm = slices[f];
        const size_t need = sm.frames * sliceSize;
        std::vector<Uint16> pixels;
        if (!readPixels(sm.path, sm.bitsAllocated, need, pixels)) {
            loadFailed.fetch_add(1);
        } else {
            for (size_t t: fileTargets[f]) {
                const Target &tg = targets[t];
                memcpy(vol->data + tg.zIndex * sliceSize,
                       pixels.data() + tg.frame * sliceSize,
                       sliceSize * sizeof(Uint16));
            }
        }
        size_t done = pixelDone.fetch_add(1) + 1;
        if (progress) progress(files.size() + done, files.size() + slices.size());
    });
    if (loadFailed > 0) {
        LOGW("loadSeries: %d slices failed to load pixels (kept as blank)", loadFailed.load());
    }

    vol->elapsedMs = nowMs() - t0;
    LOGD("loadSeries: done in %lld ms", vol->elapsedMs);
    return vol;
}

void CbctSeriesParser::release(CbctVolume *vol) {
    delete vol;
}

double CbctSeriesParser::toHu(const CbctVolume *vol, Uint16 raw) {
    double v = (vol->pixelRepresentation != 0) ? (double) (Sint16) raw : (double) raw;
    return v * vol->slope + vol->intercept;
}

uint8_t CbctSeriesParser::applyWindow(double hu, double wc, double ww) {
    // DICOM PS3.3 C.11.2.1.2 线性窗变换
    const double c = wc - 0.5;
    const double w = ww - 1.0;
    if (w <= 0.0) return hu > c ? 255 : 0;
    const double t = (hu - c) / w + 0.5;
    if (t <= 0.0) return 0;
    if (t >= 1.0) return 255;
    return (uint8_t) (t * 255.0 + 0.5);
}

bool CbctSeriesParser::extractAxial(const CbctVolume *vol, int zIndex,
                                    double ww, double wc,
                                    std::vector<uint8_t> &outRgba, int &outW, int &outH) {
    if (!vol || !vol->data) return false;
    if (zIndex < 0 || zIndex >= vol->depth) return false;

    const int w = vol->width, h = vol->height;
    const Uint16 *slice = vol->data + (size_t) zIndex * vol->sliceSize;
    outRgba.resize((size_t) w * h * 4);
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) {
            double hu = toHu(vol, slice[(size_t) r * w + c]);
            uint8_t g = applyWindow(hu, wc, ww);
            size_t o = ((size_t) r * w + c) * 4;
            outRgba[o] = g;
            outRgba[o + 1] = g;
            outRgba[o + 2] = g;
            outRgba[o + 3] = 0xFF;
        }
    }
    outW = w;
    outH = h;
    return true;
}

bool CbctSeriesParser::extractMpr(const CbctVolume *vol, MprPlane plane, int position,
                                   double ww, double wc,
                                   std::vector<uint8_t> &outRgba, int &outW, int &outH) {
    if (!vol || !vol->data) return false;

    const int w = vol->width, h = vol->height, d = vol->depth;
    if (plane == PLANE_CORONAL) {
        // 冠状面：固定 Y，输出 w × d（横向 x，纵向 z）
        if (position < 0 || position >= h) return false;
        outRgba.resize((size_t) w * d * 4);
        for (int z = 0; z < d; ++z) {
            const Uint16 *slice = vol->data + (size_t) z * vol->sliceSize;
            for (int x = 0; x < w; ++x) {
                double hu = toHu(vol, slice[(size_t) position * w + x]);
                uint8_t g = applyWindow(hu, wc, ww);
                size_t o = ((size_t) z * w + x) * 4;
                outRgba[o] = g;
                outRgba[o + 1] = g;
                outRgba[o + 2] = g;
                outRgba[o + 3] = 0xFF;
            }
        }
        outW = w;
        outH = d;
        return true;
    } else if (plane == PLANE_SAGITTAL) {
        // 矢状面：固定 X，输出 h × d（横向 y，纵向 z）
        if (position < 0 || position >= w) return false;
        outRgba.resize((size_t) h * d * 4);
        for (int z = 0; z < d; ++z) {
            const Uint16 *slice = vol->data + (size_t) z * vol->sliceSize;
            for (int y = 0; y < h; ++y) {
                double hu = toHu(vol, slice[(size_t) y * w + position]);
                uint8_t g = applyWindow(hu, wc, ww);
                size_t o = ((size_t) z * h + y) * 4;
                outRgba[o] = g;
                outRgba[o + 1] = g;
                outRgba[o + 2] = g;
                outRgba[o + 3] = 0xFF;
            }
        }
        outW = h;
        outH = d;
        return true;
    }
    return false;
}
