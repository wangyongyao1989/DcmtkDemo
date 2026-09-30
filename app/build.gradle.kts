plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.example.dcmtkdemo"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.example.dcmtkdemo"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // 本工程所有自研 .so（native-lib / cbctdeal / cbct_measure / rawpixeldeal）只编
        // arm64-v8a；而 onnxruntime-android AAR 自带 4 个 ABI，会在 APK 里多带约 57MB
        // 真机永远用不到的库。PRD 5.6 的 PC-05 限制"模型 + 运行时 <= 80MB"，因此这里
        // 显式把打包 ABI 收到 arm64-v8a（真机 AGM3-W09HN 即该 ABI）。
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        viewBinding = true
    }
}

dependencies {
    implementation(project(":dcmtk"))
    implementation(project(":rawpixeldeal"))
    implementation(project(":cbctdeal"))
    implementation(project(":cbctmeasure"))   // 测量/规划扩展层，单向依赖 :cbctdeal
    implementation(libs.appcompat)
    implementation(libs.constraintlayout)
    implementation(libs.material)
    implementation(libs.glide)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.core.ktx)
    implementation(libs.lifecycle.runtime)
    implementation(libs.lifecycle.viewmodel)

    // LeakCanary for memory leak detection (debug only)
    debugImplementation(libs.leakcanary.android)

    testImplementation(libs.junit)
    androidTestImplementation(libs.espresso.core)
    androidTestImplementation(libs.ext.junit)
}
