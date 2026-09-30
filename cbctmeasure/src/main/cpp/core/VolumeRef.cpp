// 只读体数据视图实现（业务逻辑层，纯 C++，无 JNI / 无 VTK）。
//
// 本文件是 :cbctmeasure 访问 :cbctdeal 体数据的唯一入口。
// 内存布局约定（见 CbctVolume.h）：data 为 [depth][height][width] 连续 float，
// 单位已是 HU 域，偏移 = k * sliceSize + j * width + i。

#include "include/VolumeRef.h"
#include "include/CbctVolume.h"

#include <android/log.h>
#include <algorithm>
#include <cmath>

#define TAG "CbctMeasureCore"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

void VolumeRef::bind(const CbctVolume *vol) { vol_ = vol; }

int VolumeRef::width() const { return vol_ ? vol_->width : 0; }
int VolumeRef::height() const { return vol_ ? vol_->height : 0; }
int VolumeRef::depth() const { return vol_ ? vol_->depth : 0; }

const float *VolumeRef::data() const { return vol_ ? vol_->data : nullptr; }
size_t VolumeRef::sliceSize() const { return vol_ ? vol_->sliceSize : 0; }

/**
 * 字符串元数据访问器：std::string -> const char* -> std::string 这一趟是必要的，
 * 因为 CbctVolume 里的成员是 std::string，而本头文件对外承诺不含 DCMTK 类型，
 * 空指针时统一退化为空串，调用方不必再判 valid()。
 */
static std::string metaStr(const std::string *v) { return v ? *v : std::string(); }

std::string VolumeRef::studyInstanceUID() const { return vol_ ? metaStr(&vol_->studyInstanceUID) : ""; }
std::string VolumeRef::seriesInstanceUID() const { return vol_ ? metaStr(&vol_->seriesInstanceUID) : ""; }
std::string VolumeRef::firstSlicePath() const { return vol_ ? metaStr(&vol_->firstSlicePath) : ""; }
std::string VolumeRef::patientName() const { return vol_ ? metaStr(&vol_->patientName) : ""; }
std::string VolumeRef::patientID() const { return vol_ ? metaStr(&vol_->patientID) : ""; }
std::string VolumeRef::patientSex() const { return vol_ ? metaStr(&vol_->patientSex) : ""; }
std::string VolumeRef::patientBirthDate() const { return vol_ ? metaStr(&vol_->patientBirthDate) : ""; }
std::string VolumeRef::studyDate() const { return vol_ ? metaStr(&vol_->studyDate) : ""; }
std::string VolumeRef::seriesDescription() const { return vol_ ? metaStr(&vol_->seriesDescription) : ""; }
std::string VolumeRef::manufacturer() const { return vol_ ? metaStr(&vol_->manufacturer) : ""; }

double VolumeRef::spacingX() const { return vol_ ? vol_->spacingX : 1.0; }
double VolumeRef::spacingY() const { return vol_ ? vol_->spacingY : 1.0; }
double VolumeRef::spacingZ() const { return vol_ ? vol_->spacingZ : 1.0; }

double VolumeRef::voxelVolumeMm3() const {
    return spacingX() * spacingY() * spacingZ();
}

void VolumeRef::boundsMin(Vec3 &out) const { out = Vec3(0.0, 0.0, 0.0); }

void VolumeRef::boundsMax(Vec3 &out) const {
    out = Vec3((width() - 1) * spacingX(),
               (height() - 1) * spacingY(),
               (depth() - 1) * spacingZ());
}

double VolumeRef::maxDiagonalMm() const {
    Vec3 lo, hi;
    boundsMin(lo);
    boundsMax(hi);
    return (hi - lo).length();
}

void VolumeRef::indexToWorld(int i, int j, int k, Vec3 &out) const {
    out = Vec3(i * spacingX(), j * spacingY(), k * spacingZ());
}

void VolumeRef::worldToIndex(const Vec3 &p, int &i, int &j, int &k) const {
    // spacing 恒为正（解析侧已取绝对值），这里再加一层保护避免除零
    const double sx = spacingX() > 1e-9 ? spacingX() : 1.0;
    const double sy = spacingY() > 1e-9 ? spacingY() : 1.0;
    const double sz = spacingZ() > 1e-9 ? spacingZ() : 1.0;
    i = (int) std::lround(p.x / sx);
    j = (int) std::lround(p.y / sy);
    k = (int) std::lround(p.z / sz);
}

bool VolumeRef::indexInBounds(int i, int j, int k) const {
    return vol_ != nullptr && i >= 0 && i < width() && j >= 0 && j < height() &&
           k >= 0 && k < depth();
}

bool VolumeRef::worldInBounds(const Vec3 &p) const {
    int i, j, k;
    worldToIndex(p, i, j, k);
    return indexInBounds(i, j, k);
}

float VolumeRef::at(int i, int j, int k) const {
    if (!indexInBounds(i, j, k)) return 0.0f;
    return vol_->data[(size_t) k * vol_->sliceSize + (size_t) j * vol_->width + i];
}

float VolumeRef::huNearest(const Vec3 &p, float fallback) const {
    int i, j, k;
    worldToIndex(p, i, j, k);
    if (!indexInBounds(i, j, k)) return fallback;
    return at(i, j, k);
}

float VolumeRef::huTrilinear(const Vec3 &p, float fallback) const {
    if (!vol_) return fallback;
    const double sx = spacingX() > 1e-9 ? spacingX() : 1.0;
    const double sy = spacingY() > 1e-9 ? spacingY() : 1.0;
    const double sz = spacingZ() > 1e-9 ? spacingZ() : 1.0;
    const double fx = p.x / sx, fy = p.y / sy, fz = p.z / sz;
    const int i0 = (int) std::floor(fx), j0 = (int) std::floor(fy), k0 = (int) std::floor(fz);
    // 越界一个像素以内仍允许（按钳制取值），完全飞出体积则回退默认值
    if (i0 < -1 || i0 > width() || j0 < -1 || j0 > height() || k0 < -1 || k0 > depth()) {
        return fallback;
    }
    const double tx = fx - i0, ty = fy - j0, tz = fz - k0;
    float v[2][2][2];
    for (int dz = 0; dz <= 1; ++dz) {
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                const int ii = i0 + dx, jj = j0 + dy, kk = k0 + dz;
                // 边界外侧按最近内部体素钳制，避免取样窗口在表面出现空洞
                v[dz][dy][dx] = at(std::min(std::max(ii, 0), width() - 1),
                                   std::min(std::max(jj, 0), height() - 1),
                                   std::min(std::max(kk, 0), depth() - 1));
            }
        }
    }
    double c[2][2];
    for (int dz = 0; dz <= 1; ++dz) {
        for (int dy = 0; dy <= 1; ++dy) {
            c[dz][dy] = v[dz][dy][0] * (1.0 - tx) + v[dz][dy][1] * tx;
        }
    }
    const double cy = c[0][0] * (1.0 - ty) + c[0][1] * ty;
    const double cy1 = c[1][0] * (1.0 - ty) + c[1][1] * ty;
    return (float) (cy * (1.0 - tz) + cy1 * tz);
}

/**
 * M-07 组织类型推断。
 * 分界取自 CBCT 灰度经验值（骨小梁/皮质骨的分界随个体差异较大，
 * 仅用于 UI 提示，不参与任何安全计算）。
 */
std::string VolumeRef::tissueName(float hu) {
    if (hu < -1000.0f) return "air";             // 空气外（异常低值）
    if (hu < -150.0f) return "air/marrow";       // 空气 / 骨髓腔
    if (hu < 0.0f) return "soft tissue";         // 软组织（脂肪~水以下）
    if (hu < 150.0f) return "soft tissue/fluid"; // 软组织 / 体液
    if (hu < 300.0f) return "cancellous bone";   // 骨松质
    if (hu < 1000.0f) return "cortical bone";    // 骨皮质
    if (hu < 2000.0f) return "dense bone";       // 致密骨 / 钙化
    return "metal artifact";                     // 金属 / 硬化伪影
}
