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

    // 扩展层单向依赖 :cbctdeal（共享 Volume 指针与同一渲染窗口，见模块 README 7.1）
    api(project(":cbctdeal"))

    testImplementation(libs.junit)
    androidTestImplementation(libs.espresso.core)
    androidTestImplementation(libs.ext.junit)
}
