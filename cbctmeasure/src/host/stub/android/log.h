#ifndef DCMTKDEMO_HOST_STUB_ANDROID_LOG_H
#define DCMTKDEMO_HOST_STUB_ANDROID_LOG_H

#include <cstdarg>
#include <cstdio>

/**
 * 主机侧（macOS / clang）android/log.h 桩件。
 *
 * 为什么要它：:cbctmeasure 的业务逻辑层（core 目录下的 cpp）按 Android 习惯无条件
 * `#include <android/log.h>` 并使用 __android_log_print()。PRD 第 9 章 M2/M3
 * 的验收动作要求"主机侧编译纯 C++ 核心跑单元测试"，交叉编译环境的 log.h 在
 * 主机上不存在，因此这里提供同签名实现，让同一份 core/ 源码不改一行即可
 * 在 macOS clang 下编译链接。
 *
 * 语义选择：默认"静默"（空循环体只消费参数），理由是
 *   1) 单测要人读——LOGD 高频出现在体素遍历里，全部打印会淹没断言输出；
 *   2) 保持 -Wall -Wformat 干净：用 __attribute__((format)) 让编译器仍然
 *      校验调用点的格式串（真机上写错的 %zu/%d 在主机侧同样能提前暴露），
 *      而空循环体保证参数被"使用"，不产生 unused-variable 警告。
 * 需要排查时把 CBCT_MEASURE_HOST_LOG_STDOUT 置 1 即可看到 native 日志。
 */

/* 与 NDK android/log.h 中 prio 常量取值保持一致（顺序/数值都不能改） */
#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7
#define ANDROID_LOG_SILENT 8

/** 返回值语义与真机一致：0 表示成功 */
static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...)
__attribute__((format(printf, 3, 4)));

static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef CBCT_MEASURE_HOST_LOG_STDOUT
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [%d] ", tag, prio);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
#else
    (void) prio;
    (void) tag;
    /* 只引用格式串以吃到 printf 属性带来的格式校验，运行期无任何开销 */
    if (fmt == (const char *) 0) return -1;
#endif
    return 0;
}

static inline int __android_log_write(int prio, const char *tag, const char *msg) {
#ifdef CBCT_MEASURE_HOST_LOG_STDOUT
    fprintf(stderr, "[%s] [%d] %s\n", tag, prio, msg ? msg : "");
#else
    (void) prio;
    (void) tag;
    (void) msg;
#endif
    return 0;
}

#endif /* DCMTKDEMO_HOST_STUB_ANDROID_LOG_H */
