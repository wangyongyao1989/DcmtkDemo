# DcmtkDemo — Full-Stack Medical Imaging on Android (PACS + DICOM + 3D CBCT)

🌐 **[中文](README.md)** | **English**

`DcmtkDemo` is a full-stack medical imaging sample project built for Android. It integrates **DCMTK (DICOM Toolkit)**, **VTK 9.1.0** and **OpenCV**, covering the entire chain from PACS networking and DICOM file parsing to high-performance pixel preprocessing, plus **CBCT (cone-beam CT) volume rendering and MPR reconstruction**.

> Most in-repo documents, code comments and the CSDN article series are written in Chinese. This file is the English entry point; the links below keep their original titles and are marked **(中文)** where no translation exists.

---

## 🚀 Core Features

### 1. PACS Networking (`:dcmtk`)
Implements the standard DICOM 3.0 communication protocols and talks directly to commercial PACS systems or hospital workstations:
*   **C-ECHO**: connectivity test.
*   **C-FIND**: multi-criteria query by patient name, accession number or Worklist.
*   **C-STORE**: asynchronous image upload with real-time progress callbacks.
*   **C-GET / C-MOVE**: image retrieval and download.

### 2. CBCT 3D Visualization and Series Parsing (`:cbctdeal`)
Built on **VTK 9.1.0** (trimmed Android arm64 build) and **DCMTK**:
*   **VR (Volume Rendering)**: GPU ray-casting volume rendering, 3D bone reconstruction, optional shading.
*   **MPR (Multi-Planar Reconstruction)**: axial / coronal / sagittal plane browsing with live window width and level.
*   **Series parsing**: two-pass multi-threaded parsing (JPEG / JPEG-LS supported), automatic Z-coordinate ordering, gap interpolation for missing slices.
*   **Gestures**: one-finger rotate/pan, two-finger zoom/pan, driven entirely by the Native render loop.

### 3. High-Performance Pixel Preprocessing (`:rawpixeldeal`)
Native-level processing of CT / X-Ray raw data (16-bit raw / HU):
*   **Physical correction**: log transform and HU standardisation.
*   **Smart windowing**: in-house `Peak Area Auto` algorithm that detects the tissue peak automatically.
*   **Enhancement**: bilateral denoising, CLAHE contrast enhancement, USM sharpening.

---

## 📺 Video Demos

| Demo | Description |
| :--- | :--- |
| [**vtk3D.mp4**](cbctdeal/doc/vtk3D.mp4) | CBCT series parsing + 3D bone reconstruction + gesture interaction, end to end |
| [**vtk3D-1.mp4**](cbctdeal/doc/vtk3D-1.mp4) | VR/MPR mode switching, slice browsing, live window width/level adjustment |

---

## 📚 Technical Articles and Where the Principles Come From

The implementation is documented in a three-part article series covering **project architecture → cross-compilation → server-side integration**. The table maps each article to the modules and files in this repository so you can read the code alongside it.

| # | Article | What it covers | Where it applies in this repo |
| :-- | :--- | :--- | :--- |
| 1 | [DcmtkDemo 技术解析：医学影像处理全栈方案](https://blog.csdn.net/wangyongyao1989/article/details/163419757) **(中文)** | Three-module architecture and data flow, JNI bridge design (`SafeNewStringUTF` / `JniString` RAII), PACS networking, DICOM file I/O, Kotlin coroutine orchestration, preprocessing operators and smart windowing, `Mat` address chaining, FT46 detector tuning, MWL sync-to-database | `:dcmtk` (`native-lib.cpp` / `PacsClient.cpp` / `DicomFileIO.cpp` / `ProgressScu.cpp`), `:rawpixeldeal` (`native-lib.cpp` / `MedicalCTPreprocess.cpp` / `CtSeriesProcessor.cpp` / `ImageProcessor.cpp`), the Fragments in `:app` |
| 2 | [Mac Pro 上的 DCMTK 服务搭建](https://blog.csdn.net/wangyongyao1989/article/details/162460525) **(中文)** | Building the host-side PACS and Worklist SCP from scratch: DCMTK 3.6.8 source build, `dcmqrscp.cfg`, C-ECHO / C-STORE / C-FIND / C-MOVE loopback verification, `.wl` worklist templates and binary conversion, wiring the Android client to it | "Getting Started → Step 5 Host-side PACS and Worklist services" below; client parameters in `dcmtk/src/main/java/com/example/dcmtk/utils/PacsPrefs.kt`, Worklist template in `dcmtk/src/main/assets/wlistqry/` |
| 3 | [CBCT 三维可视化实战：DCMTK + VTK 交叉编译、集成与体渲染全流程](https://blog.csdn.net/wangyongyao1989/article/details/164620462) **(中文)** | Modular trimmed cross-compile of VTK 9.1.0 for Android, DCMTK cross-compilation and the **data dictionary trap**, two-pass series parsing, zero-copy volume import, the VR/MPR pipelines, and four fatal GLES3 compatibility issues | `:cbctdeal` (`CbctSeriesParser.cpp` / `CbctVtkRenderer.cpp` / `CbctJniHelper.cpp` / `cbct-native-lib.cpp`), build script [`cbctdeal/doc/android_vtk.sh`](cbctdeal/doc/android_vtk.sh) |

**In-depth documents shipped in this repository** (Chinese; the source material for article 3):

*   [Android 平台 VTK 集成与 CBCT 三维重建技术实战](cbctdeal/Android平台VTK集成与CBCT三维重建技术实战.md) **(中文)**
*   [CBCT 原理、技术架构与临床应用深度研究报告](cbctdeal/doc/CBCT（锥形束计算机断层扫描）原理、技术架构与临床应用深度研究报告.pdf) **(中文)**
*   [Android 平台基于 DCMTK 的 CBCT DICOM 序列解析技术](cbctdeal/doc/深度研究：Android平台基于DCMTK的CBCT%20DICOM序列解析技术.pdf) **(中文)**
*   [自适应调节医学 CT 序列图像窗宽窗位算法](dcmtk/src/main/doc/自适应调节医学CT序列图像窗宽窗位算法.pdf) **(中文)** — the requirements source behind the `Peak Area Auto` windowing algorithm

---

## 🛠️ Getting Started

### 0. Versions and Dependencies

| Component | Version | Location in this repository |
| :--- | :--- | :--- |
| AGP / Gradle | 9.2.1 / 9.4.1 | `gradle/libs.versions.toml`, `gradle/wrapper/gradle-wrapper.properties` |
| compileSdk / minSdk / targetSdk | 37 / 24 / 37 | `app/build.gradle.kts` |
| CMake | 3.22.1 | `externalNativeBuild` in each module's `build.gradle.kts` |
| NDK | r25 (25.1.8937393), `android-24` API baseline | see `ANDROID_NDK` in `cbctdeal/doc/android_vtk.sh` |
| ABIs | `:dcmtk` and `:cbctdeal` are `arm64-v8a` only; `:rawpixeldeal` also builds `armeabi-v7a` | `abiFilters` per module |
| C++ standard / STL | C++11, `-frtti -fexceptions`, `c++_shared` | each module's `CMakeLists.txt` |
| DCMTK | 3.6.9 (static libs + headers committed to the repo) | `dcmtk/src/main/cpp/dcmtk/` (headers), `dcmtk/src/main/cpp/lib/arm64-v8/` (29 `.a` files) |
| VTK | 9.1.0 trimmed build (static libs + headers committed) | `cbctdeal/src/main/cpp/include/vtk-9.1/`, `cbctdeal/src/main/cpp/lib/` (43 `libvtk*.a` files) |
| OpenCV | 4.12.0 (shared library) | `rawpixeldeal/src/main/cpp/include/`, `rawpixeldeal/src/main/cpp/libs/<ABI>/libopencv_java4.so` |

> Article 2 builds the host-side service from DCMTK **3.6.8**, while the cross-compiled artefacts committed for Android are **3.6.9** (see `PACKAGE_VERSION` in `osconfig.h`). They interoperate fully at the C-ECHO / C-FIND / C-STORE / C-MOVE level, so there is no need to match minor versions for integration testing.

> **Good news: all three third-party binary toolchains are committed to this repository.** After cloning you can run `assembleDebug` straight away without cross-compiling anything. Steps 1–3 below only matter if you want to change NDK version, add VTK/DCMTK modules, or shrink the binary size.

### 1. Cross-Compile DCMTK (article 3, chapter 4)

```bash
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DBUILD_SHARED_LIBS=OFF \
  -DDCMTK_WITH_ICU=OFF -DDCMTK_WITH_XML=OFF -DDCMTK_WITH_PNG=OFF \
  -DDCMTK_WITH_TIFF=OFF -DDCMTK_WITH_ZLIB=ON \
  -DDCMTK_BUILD_SAMPLES=OFF -DDCMTK_BUILD_TESTING=OFF
```

Disabling third-party dependencies is about binary size. The choices baked into the committed build are readable straight from `dcmtk/src/main/cpp/dcmtk/config/osconfig.h`: `#define WITH_ZLIB` is active, while `WITH_LIBPNG` / `WITH_LIBTIFF` / `WITH_LIBXML` / `WITH_LIBICONV` / `WITH_OPENSSL` are all `#undef`.

Two common pitfalls: add `-latomic` when `__atomic_load_8` is reported undefined; resolve circular dependencies between static libraries (`dcmjpeg` / `dcmjpls` / `dcmimage` / `dcmimgle` / `ijg*` reference each other) with `-Wl,--start-group ... --end-group` — already done in both `dcmtk/src/main/cpp/CMakeLists.txt` and `cbctdeal/src/main/cpp/CMakeLists.txt`. Copy the output into `dcmtk/src/main/cpp/lib/arm64-v8/` (the directory name is a legacy artefact; CMake globs both `lib/${ANDROID_ABI}/` and `lib/arm64-v8/`).

### 2. Cross-Compile VTK 9.1.0 (article 3, chapter 5)

Run the shipped script [`cbctdeal/doc/android_vtk.sh`](cbctdeal/doc/android_vtk.sh) after pointing `ANDROID_NDK` and `SRC_DIR` at your own paths. Key trimming points:

*   **Disable groups first, then enable modules.** VTK 9.1's top-level CMakeLists sets the `StandAlone` and `Rendering` groups to `WANT` by default, so per-module `=NO` has no effect. You must pass `-DVTK_GROUP_ENABLE_StandAlone=DONT_WANT -DVTK_GROUP_ENABLE_Rendering=DONT_WANT` and then explicitly `=YES` the modules you need, otherwise the build degrades into a full build of several hundred libraries.
*   **VTK 9.1.0 has no `VolumeRendering` or `MPR` module** (those names belong to 9.2+). Volume rendering is split into `RenderingVolume` + `RenderingVolumeOpenGL2`; MPR is implemented with `vtkImageReslice` from `ImagingCore` displayed by `vtkImageActor` from `RenderingImage`.
*   Output: flattened headers in `include/vtk-9.1/` plus `lib/libvtk*.a`. Link them with `--start-group` for the same circular-dependency reason.

### 3. Inject the DCMTK Data Dictionary (article 3, section 4.4 — **the trap, read this**)

The cross-compiled DCMTK has no dictionary compiled in. The symptom is misleading: **uncompressed series parse fine, and only JPEG / JPEG-LS compressed series fail with `readPixels: all access methods failed`, because the decompression path queries the VR of standard tags from an empty dictionary.**

You can confirm the state directly from `dcmtk/src/main/cpp/dcmtk/config/osconfig.h` in this repo: `#define DCM_DICT_DEFAULT 2` (load the **external file dictionary** at startup rather than a built-in one), `/* #undef ENABLE_PRIVATE_TAGS */` (private dictionary not compiled in), and `DCM_DICT_DEFAULT_PATH` still points at a leftover Windows path on the build machine, `C:/Users/Fall/Desktop/dcmtk/install-android-arm64/share/dcmtk-3.6.9/dicom.dic`, which obviously cannot exist on Android. So the app must supply the dictionary itself — two things are required together:

1.  Ship `dicom.dic` in module assets, copy it to internal storage before the first parse and inject it — see `ensureDictionary()` in `cbctdeal/src/main/java/com/wangyao/cbctdeal/engine/CbctParseEngine.kt` and `initDictionary()` in `cbctdeal/src/main/cpp/cbct-native-lib.cpp`:

    ```kotlin
    CbctJni.initDictionary(File(context.filesDir, "dicom.dic").absolutePath)
    ```

2.  Keep `-ffunction-sections -fdata-sections` in `CMakeLists.txt` but **do not enable `--gc-sections`** — the dictionary is a lazily initialised static data table, so the linker reclaims it as an unreferenced section and the injection becomes useless (see the comment in `cbctdeal/src/main/cpp/CMakeLists.txt`).

### 4. Build and Install the App

```bash
./gradlew :app:assembleDebug            # add --offline once dependencies are cached
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.example.dcmtkdemo/.activity.MainActivity
```

3D visualisation (VR / MPR) requires OpenGL ES 3.0+ and EGL, so it **must be verified on a real device**; emulators usually cannot provide a usable GPU ray-casting context.

### 5. Host-Side PACS and Worklist Services (article 2 — needed for the networking features)

Upload / Query / Retrieve / Worklist all require a real SCP. On a Mac, follow article 2:

1.  Build and install DCMTK from source: `dcmtk-3.6.8.tar.gz` → `mkdir build && cd build` → `cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/dcmtk-3.6.8-install -DCMAKE_CXX_STANDARD=11 -DBUILD_SHARED_LIBS=ON -DDCMTK_WITH_WLSTORAGE=ON` → `make -j$(sysctl -n hw.ncpu) && make install` → append `bin` to `PATH` in `~/.zshrc`.
2.  Create the directories and write `dcmqrscp.cfg`. **Use ASCII-only paths and create every directory up front** — a missing directory makes the service fail silently (no error, but nothing is stored):

    ```
    DCMTK_PACS_TEST/
    ├─ SCP/  ├─ database/      # PACS storage database (create in advance)
    │        └─ dcmqrscp.cfg
    └─ SCU/  ├─ test.dcm
             └─ recv_db/        # C-MOVE destination directory (create in advance)
    ```

    The three critical lines: `NetworkTCPPort 11112`; in `HostTable`, whitelist the **LAN IP of the Android device** (`android_client = (ANDROID_SCU, 192.168.1.11, 1234)`, where 1234 is the C-MOVE receive port); in `AETable`, `ACME_STORE <absolute path to database> RW (9, 1024mb) acmeCTcompany`.
3.  Start the service from the `SCP/` directory: `dcmqrscp -d --config dcmqrscp.cfg 11112`.
4.  Verify with the command-line tools before touching Android: `echoscu` (expect `Received Echo Response (Success)`) → `storescu ... +sd test.dcm` (expect `DIMSE Status 0x0000: Success`) → `findscu -k QueryRetrieveLevel=STUDY -k StudyInstanceUID=` → `movescu --port 1234 -od .../recv_db`. Retrieved `.dcm` files should appear in `recv_db/`.
5.  The app parameters map one-to-one onto the server. Defaults live in `dcmtk/src/main/java/com/example/dcmtk/utils/PacsPrefs.kt`: Remote AE = `ACME_STORE`, Local AE = `ANDROID_SCU`, Port = `11112`; the Worklist Remote AE defaults to `OFFIS`. Changing the IP needs no rebuild — just type it in the UI. The phone and the server must be on the **same LAN**.
6.  Worklist data: edit a worklist template → convert it to a binary `.wl` → verify with `findscu`. A sample ships at `dcmtk/src/main/assets/wlistqry/wlistqry1.wl`. Query results are synced into the local database (two tables, `patient` / `study`, upserted in a single transaction); see chapter 10 of article 1.

### 6. Test Data and One Easy Mistake

All demo data is bundled under `app/src/main/assets/`: the `.raw` / `.bin` files used by CT Preprocess, several `.dcm` files, and `neck_ct/` (a 265-slice CBCT series used by the "Load ASSETS/NECK_CT" button in `:cbctdeal`).

> ⚠️ **Pick the right byte order on the CT Preprocess screen**: `.raw` files need **Little**, `.bin` files need **Big**. Read with the wrong order and the pixel values scatter across 0–65535, the histogram looks roughly uniform and auto-windowing degenerates to full range — that is a **byte-order symptom, not an algorithm defect**. Also note the W/H inputs are not linked to the selected file: 1112x1740 against a 1112x1700 `CR*.raw` asks for 40 rows too many, and the Native side truncates by row and logs a `RawPixelDealJni: ... truncate to 1700 rows` warning, which is expected.

---

## 🏗️ Architecture

*   **`:app`**: presentation layer, async work orchestrated with coroutines.
*   **`:cbctdeal`**: **the 3D core**. VTK 9.1.0 static libraries plus the C++ rendering engine.
*   **`:dcmtk`**: communication layer. Wraps the DCMTK static libraries for networking and file I/O.
*   **`:rawpixeldeal`**: algorithm layer. Low-level pixel transforms on OpenCV.

Three-layer separation is the design principle running through the whole project: **the JNI bridge layer is the only place that touches `JNIEnv` and does nothing but type marshalling; the business layer (`PacsClient` / `DicomFileIO` / `CtSeriesProcessor`) uses plain C++ types, has no JNI dependency, and can be read and tested in isolation.** See chapters 2 and 5 of article 1.

### Feature Entry Points (8 items in the left drawer, defined in `app/src/main/res/menu/bottom_nav_menu.xml`)

| Entry | Fragment | Module(s) | Getting-started note | Principle |
| :--- | :--- | :--- | :--- | :--- |
| Upload | `UploadFragment` | `:dcmtk` | Start `dcmqrscp` per step 5 first; C-STORE reports progress live | Article 1 ch. 3; article 2 §2.3 |
| Query | `QueryFragment` | `:dcmtk` | Query by name / accession number / StudyUID | Article 1 ch. 3; article 2 §2.4 |
| Worklist | `WorklistQueryFragment` | `:dcmtk` | Remote AE defaults to `OFFIS`; results sync to the local DB | Article 1 ch. 10; article 2 §3 |
| Retrieve | `RetrieveFragment` | `:dcmtk` | C-MOVE needs the `1234` receive port from `HostTable` reachable | Article 1 ch. 3; article 2 §2.5 |
| Show | `DcmShowFragment` | `:dcmtk` | Browse local `.dcm` files with pinch zoom | Article 1 ch. 4 |
| Compare | `FileCompareFragment` | `:dcmtk` + `:rawpixeldeal` | Compare a DICOM file against windowing results | Article 1 ch. 4 and 7 |
| CT Preprocess | `CTPreprocessFragment` | `:rawpixeldeal` | Pick asset → pick byte order → optimal adjustment / custom pipeline / write DICOM | Article 1 ch. 6, 7, 9; `rawpixeldeal/README.md` (中文) |
| CBCT Parse | `CbctParseFragment` | `:cbctdeal` | "Load ASSETS/NECK_CT" gives parsing + Bitmap 2D + VTK VR / MPR in one tap | Article 3 ch. 7, 8, 9; `cbctdeal/README.md` (中文) |

---

## 🤝 Summary

By pushing heavy image algorithms and parsing logic down into the Native layer, this project delivers desktop-workstation-class medical imaging capability on Android. It is a reference implementation for **mobile physician workstations, dental/craniofacial imaging viewers and teleradiology apps**.
