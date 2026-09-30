// ONNX Runtime 的 dlopen + OrtApi C 函数表实现。
//
// 设计要点见 ai/OrtEngine.h 顶部注释。本文件是唯一 include ORT 头文件的
// 翻译单元（因此主机侧单测不编译它，CMake 里也只进 Android 目标）。

#include "ai/OrtEngine.h"

#include "third_party/onnxruntime/include/onnxruntime_c_api.h"

#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

// 本模块自己的日志（真机侧；与 core/ 下同一 TAG 族）
#include <android/log.h>

#define TAG "CbctMeasureAi"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {
    typedef const OrtApiBase *(*GetApiBaseFn)(void);

    /** 只取 dlopen 之后仍然有效的最小状态；ORT 的指针全部是不透明句柄 */
    struct EngineState {
        void *handle = nullptr;
        const OrtApiBase *base = nullptr;
        const OrtApi *api = nullptr;
        uint32_t apiVersion = 0;
        OrtEnv *env = nullptr;
        OrtSessionOptions *options = nullptr;
        OrtRunOptions *runOptions = nullptr;
        OrtMemoryInfo *memInfo = nullptr;
        OrtAllocator *alloc = nullptr;
        OrtSession *session = nullptr;
        std::string version;

        void clearStatus(OrtStatus *st, std::string *into) {
            if (!st) return;
            if (into && api) {
                const char *m = api->GetErrorMessage(st);
                *into = m ? m : "(ORT 未给出错误文本)";
            }
            if (api) api->ReleaseStatus(st);
        }
    };
}   // namespace

struct OrtEngine::Impl : EngineState {};

OrtEngine::OrtEngine() : impl_(new Impl()) {
    inDim_[0] = inDim_[1] = inDim_[2] = 0;
    inChannels_ = outChannels_ = 0;
    session_ = nullptr;
    soLoaded_ = false;
}

OrtEngine::~OrtEngine() {
    unload();
    delete impl_;
}

std::string OrtEngine::info() const {
    if (!impl_->api) return "onnxruntime: 未加载";
    char buf[256];
    snprintf(buf, sizeof(buf), "onnxruntime %s / C API v%u / in '%s' [%d,%d,%d,%d] / out '%s'",
             impl_->version.empty() ? "?" : impl_->version.c_str(), impl_->apiVersion,
             inName_.c_str(), inChannels_, inDim_[0], inDim_[1], inDim_[2], outName_.c_str());
    return std::string(buf);
}

bool OrtEngine::inputDim3(int dim[3]) const {
    if (!session_) return false;
    dim[0] = inDim_[0];
    dim[1] = inDim_[1];
    dim[2] = inDim_[2];
    return dim[0] > 0 && dim[1] > 0 && dim[2] > 0;
}

void OrtEngine::unload() {
    Impl *s = impl_;
    if (!s) return;
    // 先无条件关掉已打开的 so：dlopen 成功但 OrtGetApiBase 缺失时，
    // 下面的 api==nullptr 早退分支不能再把句柄漏在场上。
    if (s->handle) {
        dlclose(s->handle);
        s->handle = nullptr;
    }
    usedSo_.clear();
    if (!s->api) {
        session_ = nullptr;
        soLoaded_ = false;
        return;
    }
    const OrtApi *api = s->api;
    if (s->session) { api->ReleaseSession(s->session); s->session = nullptr; }
    if (s->runOptions) { api->ReleaseRunOptions(s->runOptions); s->runOptions = nullptr; }
    if (s->options) { api->ReleaseSessionOptions(s->options); s->options = nullptr; }
    if (s->memInfo) { api->ReleaseMemoryInfo(s->memInfo); s->memInfo = nullptr; }
    if (s->env) { api->ReleaseEnv(s->env); s->env = nullptr; }
    // alloc 是"默认分配器"，ORT 文档说明由库自己持有，不能 Release
    s->alloc = nullptr;
    api = nullptr;
    inName_.clear();
    outName_.clear();
    usedSo_.clear();
    inDim_[0] = inDim_[1] = inDim_[2] = 0;
    inChannels_ = outChannels_ = 0;
}

/**
 * 把 ';' 分隔的候选串拆开，并补一条"裸 SONAME"兜底。
 *
 * 为什么需要裸名：app 自己的 linker namespace 的搜索路径里就带着
 * base.apk!/lib/<abi>（这正是 ClassLoader 找到本模块 .so 的机制），
 * 所以按名解析有可能直接命中原位映射的那份库，即使绝对路径写法被拒。
 */
static std::vector<std::string> soCandidates(const std::string &joined) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos <= joined.size()) {
        const size_t nxt = joined.find(';', pos);
        std::string one = (nxt == std::string::npos) ? joined.substr(pos)
                                                     : joined.substr(pos, nxt - pos);
        if (!one.empty()) v.push_back(one);
        if (nxt == std::string::npos) break;
        pos = nxt + 1;
    }
    const std::string bare = "libonnxruntime.so";
    bool has = false;
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == bare) has = true;
    if (!has) v.push_back(bare);
    return v;
}

bool OrtEngine::load(const std::string &soPath, const std::string &modelPath,
                     int threads, std::string &err) {
    unload();
    err.clear();
    Impl *s = impl_;

    // ---- 1) 打开随 AAR 打进 APK 的 libonnxruntime.so ----
    //
    // 三种形态都试，因为"so 到底在磁盘上还是在 APK 里"取决于打包方式
    // （jniLibs.useLegacyPackaging / android:extractNativeLibs），不是代码能假设的：
    //   a) nativeLibraryDir/libonnxruntime.so      —— 安装期解到磁盘，stat 能过
    //   b) <apk>!/lib/<abi>/libonnxruntime.so       —— 原位映射，不是文件系统路径，
    //      因此含 "!/" 或相对名一律跳过 stat，直接交给 linker 判定
    //   c) libonnxruntime.so                        —— 走 app namespace 搜索路径
    const std::vector<std::string> cands = soCandidates(soPath);
    std::string reasons;
    for (size_t i = 0; i < cands.size(); ++i) {
        const std::string &p = cands[i];
        const bool inApkOrRelative =
            (p.find("!/") != std::string::npos) || (!p.empty() && p[0] != '/');
        struct stat st;
        if (!inApkOrRelative && stat(p.c_str(), &st) != 0) {
            reasons += " | " + p + ": 不是磁盘上的文件";
            continue;
        }
        s->handle = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (s->handle) {
            usedSo_ = p;
            LOGD("dlopen ok: %s", p.c_str());
            break;
        }
        const char *d = dlerror();
        reasons += " | " + p + ": " + (d ? d : "?");
        LOGW("dlopen 候选 %zu 失败: %s", i + 1, reasons.c_str());
    }
    if (!s->handle) {
        err = "libonnxruntime.so 打开失败（试过 " + std::to_string(cands.size()) +
              " 个候选）:" + reasons;
        LOGE("%s", err.c_str());
        return false;
    }
    soLoaded_ = true;

    const GetApiBaseFn getBase = (const GetApiBaseFn) dlsym(s->handle, "OrtGetApiBase");
    if (!getBase) {
        err = "符号 OrtGetApiBase 不存在（该 .so 不是 ONNX Runtime？）";
        LOGE("%s", err.c_str());
        unload();
        return false;
    }
    s->base = getBase();
    if (!s->base) {
        err = "OrtGetApiBase() 返回空";
        unload();
        return false;
    }
    // 头文件写的是 ORT_API_VERSION=17；设备上的 so 若更旧会返回 nullptr，
    // 因此从期望版本往下找"两端都支持的最高版本"。低版本 API 结构体是
    // 高版本的前缀（ORT 的 ABI 约定），本类用到的调用都在最早的版本段里。
    for (uint32_t v = ORT_API_VERSION; v >= 1 && !s->api; --v) {
        s->api = s->base->GetApi(v);
        if (s->api) s->apiVersion = v;
    }
    if (!s->api) {
        err = "ORT C API 版本不兼容（期望 >=1，头文件 " + std::to_string(ORT_API_VERSION) + "）";
        unload();
        return false;
    }
    const char *vs = s->base->GetVersionString();
    s->version = vs ? vs : "";
    if (s->apiVersion != (uint32_t) ORT_API_VERSION) {
        LOGW("ORT API 降级使用 v%u（头文件 v%d，so=%s）",
             s->apiVersion, ORT_API_VERSION, s->version.c_str());
    }
    const OrtApi *api = s->api;

    struct stat mst;
    if (stat(modelPath.c_str(), &mst) != 0) {
        err = "找不到模型文件: " + modelPath;
        LOGE("%s", err.c_str());
        unload();
        return false;
    }
    LOGD("模型 %s（%lld 字节），运行时 so=%s，ORT %s api v%u",
         modelPath.c_str(), (long long) mst.st_size, usedSo_.c_str(),
         s->version.c_str(), s->apiVersion);

    // ---- 2) Env / SessionOptions ----
    OrtStatus *status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "CbctMeasureAi", &s->env);
    if (status) { s->clearStatus(status, &err); unload(); return false; }
    status = api->CreateSessionOptions(&s->options);
    if (status) { s->clearStatus(status, &err); unload(); return false; }
    if (api->SetIntraOpNumThreads(s->options, threads > 0 ? threads : 4) != nullptr) {
        LOGW("SetIntraOpNumThreads 失败，沿用默认线程数");
    }
    if (api->SetInterOpNumThreads(s->options, 1) != nullptr) {
        LOGW("SetInterOpNumThreads 失败，沿用默认线程数");
    }
    // 全量图优化：融合 Conv+Relu、把 Softmax 合成单 kernel，平板上实测影响首帧耗时，
    // 因此放在加载期一次性做完（CreateSession 内部跑），不摊到每次推理。
    if (api->SetSessionGraphOptimizationLevel) {
        if (api->SetSessionGraphOptimizationLevel(s->options, ORT_ENABLE_ALL) != nullptr) {
            LOGW("SetSessionGraphOptimizationLevel 失败，沿用默认级别");
        }
    }

    // ---- 3) 建会话（模型文件解析在这里）----
    status = api->CreateSession(s->env, modelPath.c_str(), s->options, &s->session);
    if (status) {
        std::string ortErr;
        s->clearStatus(status, &ortErr);
        err = "模型加载失败（opset/算子不被设备端 ORT 支持？）: " + ortErr;
        LOGE("%s", err.c_str());
        unload();
        return false;
    }
    session_ = s->session;

    // ---- 4) 读输入/输出名与形状（模型是网格尺寸的权威）----
    status = api->GetAllocatorWithDefaultOptions(&s->alloc);
    if (status) { s->clearStatus(status, &err); unload(); return false; }

    size_t nIn = 0, nOut = 0;
    if ((status = api->SessionGetInputCount(s->session, &nIn)) ||
        (status = api->SessionGetOutputCount(s->session, &nOut))) {
        s->clearStatus(status, &err);
        err = "读取输入/输出数量失败: " + err;
        unload();
        return false;
    }
    if (nIn != 1 || nOut != 1) {
        char buf[128];
        snprintf(buf, sizeof(buf), "本模块只支持单入单出模型（实际输入 %zu 输出 %zu）", nIn, nOut);
        err = buf;
        unload();
        return false;
    }
    char *rawIn = nullptr, *rawOut = nullptr;
    if ((status = api->SessionGetInputName(s->session, 0, s->alloc, &rawIn)) ||
        (status = api->SessionGetOutputName(s->session, 0, s->alloc, &rawOut))) {
        s->clearStatus(status, &err);
        err = "读取张量名失败: " + err;
        if (rawIn) s->alloc->Free(s->alloc, rawIn);
        if (rawOut) s->alloc->Free(s->alloc, rawOut);
        unload();
        return false;
    }
    if (rawIn) inName_ = rawIn;
    if (rawOut) outName_ = rawOut;
    if (rawIn) s->alloc->Free(s->alloc, rawIn);
    if (rawOut) s->alloc->Free(s->alloc, rawOut);

    // 形状
    int64_t inDims[8] = {0}, outDims[8] = {0};
    size_t inRank = 0, outRank = 0;
    const OrtTensorTypeAndShapeInfo *tin = nullptr, *tout = nullptr;
    OrtTypeInfo *tiIn = nullptr, *tiOut = nullptr;
    bool shapeOk = true;
    if ((status = api->SessionGetInputTypeInfo(s->session, 0, &tiIn))) shapeOk = false;
    if (shapeOk && (status = api->CastTypeInfoToTensorInfo(tiIn, &tin))) shapeOk = false;
    if (shapeOk && (status = api->GetDimensionsCount(tin, &inRank))) shapeOk = false;
    if (shapeOk) {
        size_t want = inRank < 8 ? inRank : 8;
        if ((status = api->GetDimensions(tin, inDims, want))) shapeOk = false;
    }
    api->ReleaseTypeInfo(tiIn);
    tiIn = nullptr;
    if ((status = api->SessionGetOutputTypeInfo(s->session, 0, &tiOut))) shapeOk = false;
    if (shapeOk && (status = api->CastTypeInfoToTensorInfo(tiOut, &tout))) shapeOk = false;
    if (shapeOk && (status = api->GetDimensionsCount(tout, &outRank))) shapeOk = false;
    if (shapeOk) {
        size_t want = outRank < 8 ? outRank : 8;
        if ((status = api->GetDimensions(tout, outDims, want))) shapeOk = false;
    }
    api->ReleaseTypeInfo(tiOut);
    tiOut = nullptr;
    s->clearStatus(status, &err);
    if (!shapeOk || !err.empty()) {
        err = "读取张量形状失败: " + (err.empty() ? "未知错误" : err);
        LOGE("%s", err.c_str());
        unload();
        return false;
    }
    if (inRank != 5) {
        char buf[128];
        snprintf(buf, sizeof(buf), "模型输入应为 5D(NCDHW)，实际 rank=%zu", inRank);
        err = buf;
        LOGE("%s", err.c_str());
        unload();
        return false;
    }
    inChannels_ = (int) (inDims[1] > 0 ? inDims[1] : 0);
    // 动态轴（<=0 或 -1）在此不允许：网格映射依赖确定的模型尺寸
    if (inDims[2] <= 0 || inDims[3] <= 0 || inDims[4] <= 0) {
        err = "模型输入的空间维度是动态轴，本模块要求静态 [1,C,X,Y,Z]";
        LOGE("%s", err.c_str());
        unload();
        return false;
    }
    inDim_[0] = (int) inDims[2];
    inDim_[1] = (int) inDims[3];
    inDim_[2] = (int) inDims[4];
    outChannels_ = (outRank >= 2 && outDims[1] > 0) ? (int) outDims[1] : 0;

    status = api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &s->memInfo);
    if (status) { s->clearStatus(status, &err); unload(); return false; }
    if ((status = api->CreateRunOptions(&s->runOptions))) {
        s->clearStatus(status, &err);
        unload();
        return false;
    }

    // 把 ONNX 原始 shape 与解析出的逻辑维度分开打印：只挑两个下标拼进一行会读出
    // "[1,96,96,64,64]" 这种自相矛盾的假 shape，排查模型契约时极易误判。
    LOGD("ORT 就绪：in='%s' 原始shape[%lld,%lld,%lld,%lld,%lld] -> 逻辑 C%d × X%d × Y%d × Z%d；"
         "out='%s' rank=%zu 通道 in/out=%d/%d",
         inName_.c_str(),
         (long long) inDims[0], (long long) inDims[1], (long long) inDims[2],
         (long long) inDims[3], (long long) inDims[4],
         inChannels_, inDim_[0], inDim_[1], inDim_[2],
         outName_.c_str(), outRank, inChannels_, outChannels_);
    return true;
}

bool OrtEngine::infer(const float *feat, long long featCount,
                      std::vector<float> &prob, std::string &err) {
    err.clear();
    prob.clear();
    Impl *s = impl_;
    if (!s->api || !s->session) {
        err = "ORT 会话未建立";
        return false;
    }
    if (!feat || featCount <= 0) {
        err = "输入特征为空";
        return false;
    }
    const OrtApi *api = s->api;
    const long long expect = (long long) inChannels_ * inDim_[0] * inDim_[1] * inDim_[2];
    if (featCount != expect) {
        char buf[160];
        snprintf(buf, sizeof(buf), "输入长度 %lld != 模型期望 %lld", featCount, expect);
        err = buf;
        return false;
    }

    int64_t shape[5] = {1, inChannels_, inDim_[0], inDim_[1], inDim_[2]};
    OrtValue *input = nullptr;
    OrtStatus *status = api->CreateTensorWithDataAsOrtValue(
            s->memInfo, const_cast<float *>(feat), (size_t) featCount * sizeof(float),
            shape, 5, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);
    if (status) {
        s->clearStatus(status, &err);
        err = "创建输入张量失败: " + err;
        return false;
    }

    const char *inNames[1] = {inName_.c_str()};
    const OrtValue *inVals[1] = {input};
    const char *outNames[1] = {outName_.c_str()};
    OrtValue *outVal = nullptr;
    status = api->Run(s->session, s->runOptions, inNames, inVals, 1, outNames, 1, &outVal);
    if (status || !outVal) {
        s->clearStatus(status, &err);
        err = err.empty() ? "Run 返回空输出" : ("Run 失败: " + err);
        api->ReleaseValue(input);
        LOGE("%s", err.c_str());
        return false;
    }
    api->ReleaseValue(input);

    // 输出长度以实际张量为准（动态 batch 时也只可能是 1）
    OrtTensorTypeAndShapeInfo *oinfo = nullptr;
    size_t elems = 0;
    status = api->GetTensorTypeAndShape(outVal, &oinfo);
    if (!status) status = api->GetTensorShapeElementCount(oinfo, &elems);
    if (status || elems == 0) {
        s->clearStatus(status, &err);
        err = err.empty() ? "输出张量长度为 0" : ("读取输出形状失败: " + err);
        if (oinfo) api->ReleaseTensorTypeAndShapeInfo(oinfo);
        api->ReleaseValue(outVal);
        return false;
    }
    void *ptr = nullptr;
    status = api->GetTensorMutableData(outVal, &ptr);
    if (status || !ptr) {
        s->clearStatus(status, &err);
        err = err.empty() ? "输出数据指针为空" : ("取输出数据失败: " + err);
        api->ReleaseTensorTypeAndShapeInfo(oinfo);
        api->ReleaseValue(outVal);
        return false;
    }
    prob.assign((const float *) ptr, (const float *) ptr + elems);
    api->ReleaseTensorTypeAndShapeInfo(oinfo);
    api->ReleaseValue(outVal);
    if (outChannels_ > 0) {
        const long long per = (long long) inDim_[0] * inDim_[1] * inDim_[2];
        if (per > 0 && (long long) prob.size() % per != 0) {
            err = "输出元素数不是模型网格体积的整数倍";
            return false;
        }
    }
    LOGD("ORT Run ok：输出 %zu 个 float", prob.size());
    return true;
}
