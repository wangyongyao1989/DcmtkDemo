plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "com.example.rawpixeldeal"
    compileSdk = 37

    defaultConfig {
        minSdk = 24

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        consumerProguardFiles("consumer-rules.pro")

        externalNativeBuild {
            cmake {
                cppFlags("-std=c++11 -frtti -fexceptions")
                abiFilters += listOf("arm64-v8a", "armeabi-v7a")
                arguments("-DANDROID_STL=c++_shared")
            }
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

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // OpenCV 是动态库 (libopencv_java4.so)，
    // 把它放在 src/main/cpp/libs/<abi>/ 下需要额外告诉 AGP 把它打包进 APK。
    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/cpp/libs")
        }
    }
}

dependencies {
    implementation(libs.appcompat)
    implementation(libs.core.ktx)
    implementation(libs.material)
    // dcmtk 模块：复用 ProcessPixelData 调窗算法 + DicomManager.writeDcmFile
    // 1) xray 包（XrayPipeline.processXrayFromAssets）走 dcmtk.ProcessPixelData 算窗位窗宽
    // 2) 写 DCM 文件走 dcmtk.DicomManager.writeDcmFile
    implementation(project(":dcmtk"))
    testImplementation(libs.junit)
    androidTestImplementation(libs.espresso.core)
    androidTestImplementation(libs.ext.junit)
}
