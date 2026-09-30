package com.wangyao.cbctmeasure.jni

/**
 * libcbct_measure.so 的唯一加载入口。
 *
 * 为什么不各 JNI object 各自 `System.loadLibrary`（:cbctdeal 的写法）：
 * 本库在 JNI_OnLoad 里一次性给 5 个类做 RegisterNatives，若加载动作分散在
 * 这 5 个类的 static 初始化里，理论上会出现"A 类初始化触发 dlopen，
 * JNI_OnLoad 又去找 B 类"的重入路径。ART 的 FindClass 不会触发 <clinit>，
 * 所以现状能工作，但把加载收敛到一个"不被注册表引用"的类里可以彻底断掉这条链，
 * 也让"库加载失败"只有一个捕获点。
 */
internal object MeasureNative {

    @JvmStatic
    @Synchronized
    fun ensureLoaded() {
        // 幂等：重复 loadLibrary 同一库由 Runtime 记账，不会二次 dlopen
        System.loadLibrary("cbct_measure")
    }
}
