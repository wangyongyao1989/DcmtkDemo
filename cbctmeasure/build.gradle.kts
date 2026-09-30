plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "com.wangyao.cbctmeasure"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        // 与 :cbctdeal / :app 保持一致（避免清单合并时 minSdk 冲突）
        minSdk = 24

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                cppFlags("-std=c++11 -frtti -fexceptions")
                // 与 :cbctdeal 一致：体数据渲染与测量仅面向 arm64 真机
                abiFilters += "arm64-v8a"
                arguments("-DANDROID_STL=c++_shared")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    implementation(libs.appcompat)
    implementation(libs.core.ktx)
    implementation(libs.material)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.lifecycle.viewmodel)

    // ONNX Runtime Android（PRD 5.6 的 AI 层运行时）。
    //
    // 这里只用它的"交付物"，不用它的 Java API：AAR 里的 jni/<abi>/libonnxruntime.so
    // 由 AGP 打进 APK，C++ 侧 ai/OrtEngine.cpp 用 dlopen + OrtApi 跳转表调用。
    //
    // 记入档的偏差（PRD 字面写的是 Prefab，实测不可用）：
    // onnxruntime-android 1.17.0 的 AAR 内没有 prefab/ 元数据（解压后只有
    // AndroidManifest.xml / classes.jar / headers/ / jni/），所以 CMake 既不能
    // find_package(onnxruntime CONFIG) 也没有 imported 目标；把它的 C 头 vendored
    // 进 third_party/onnxruntime/include + 运行时 dlopen，是这种 AAR 形态下
    // 唯一不依赖 Java 层、又不把 16MB .so 静态链接进本模块的做法。
    implementation(libs.onnxruntime.android)

    // 扩展层单向依赖 :cbctdeal（共享 Volume 指针与同一渲染窗口，见模块 README 7.1）
    api(project(":cbctdeal"))

    testImplementation(libs.junit)
    androidTestImplementation(libs.espresso.core)
    androidTestImplementation(libs.ext.junit)
}
