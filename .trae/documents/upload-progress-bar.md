# Upload Progress Bar for C-STORE

## Context

The `UploadFragment` "Upload to PACS" flow currently shows an **indeterminate** spinning `ProgressBar` during the C-STORE upload — no byte count, no percentage. The user wants to know whether DCMTK exposes upload progress, and if so, replace the spinner with a determinate progress bar.

**Finding:** DCMTK's `DcmSCU` class ([scu.h#L947-L954](file:///d:/TestDemo/DcmtkDemo/app/src/main/cpp/include/dcmtk/dcmnet/scu.h#L947-L954)) provides a virtual callback:

```cpp
/// Called while sending DIMSE messages, i.e. on each PDV of a dataset.
virtual void notifySENDProgress(const unsigned long byteCount);
```

`byteCount` is the cumulative bytes sent so far. Progress notification is **enabled by default** (`setProgressNotificationMode`). So byte-level progress **can** be surfaced to the UI by subclassing `DcmSCU` and overriding `notifySENDProgress()`.

## Approach

Subclass `DcmSCU` in native code, override `notifySENDProgress()` to call back into Java via JNI, and drive a determinate horizontal `ProgressBar` + byte-count text in `UploadFragment`. The existing `cStore` native method signature is extended with a nullable `ProgressCallback` parameter (only caller is `UploadFragment`, so no compat shims needed).

## Files to Modify

### 1. New: `app/src/main/java/com/example/dcmtkdemo/ProgressCallback.java`
Java callback interface:
```java
public interface ProgressCallback {
    void onProgress(long sent, long total);
}
```

### 2. `app/src/main/java/com/example/dcmtkdemo/DcmtkJni.java`
- Add `ProgressCallback` interface (or separate file).
- Change `cStore` signature to add `ProgressCallback callback` (nullable):
  ```java
  public static native boolean cStore(String host, int port, String localAET,
          String remoteAET, String dcmPath, ProgressCallback callback);
  ```

### 3. `app/src/main/cpp/native-lib.cpp`
- Add `ProgressSCU : public DcmSCU` subclass holding `JNIEnv*`, `jobject callback`, `unsigned long totalBytes`. Override `notifySENDProgress(byteCount)` to call `callback.onProgress(sent, total)` via JNI. The `jobject` is a local ref valid for the duration of the synchronous `sendSTORERequest` call (same thread), so no global ref needed.
- Modify `native_cStore` to:
  - Accept `jobject callback` parameter.
  - Get file size via `stat()` for `totalBytes`.
  - Use `ProgressSCU` instead of `DcmSCU`; set its callback fields only when `callback != nullptr`.
- Update JNI method registration: change `cStore` signature to `(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Lcom/example/dcmtkdemo/ProgressCallback;)Z`.

### 4. `app/src/main/res/layout/fragment_upload.xml`
- Remove the floating circular `ProgressBar` from the `FrameLayout` root.
- Add a **horizontal determinate** `ProgressBar` inside the `LinearLayout` (below `btn_upload`, above `ScrollView`):
  ```xml
  <ProgressBar
      android:id="@+id/progress_bar"
      style="?android:attr/progressBarStyleHorizontal"
      android:layout_width="match_parent"
      android:layout_height="wrap_content"
      android:layout_marginTop="8dp"
      android:max="100"
      android:progress="0"
      android:visibility="gone" />
  ```

### 5. `app/src/main/java/com/example/dcmtkdemo/UploadFragment.java`
- In `uploadDicom()`: set `progressBar` to `max=100`, `progress=0`, visible; pass a `ProgressCallback` to `cStore`.
- The callback (invoked on worker thread) posts to UI thread via `runOnUiThread`:
  - Compute `percent = sent * 100 / total` (clamp to 100).
  - Update `binding.progressBar.setProgress(percent)`.
  - Update `binding.tvUploadStatus` with `"Uploading: X KB / Y KB (Z%)"` via a `formatBytes()` helper.
- On completion: hide progress bar, show success/failure status as before.

## Notes & Trade-offs

- **byteCount accuracy:** `notifySENDProgress` reports dataset bytes sent over the wire. The file size is an approximation — actual transmitted bytes may differ slightly due to transfer-syntax conversion (dataset conversion mode is enabled) and DICOM PDV framing. Percentage is clamped to 100% so the bar never overflows; final completion is signaled by the C-STORE response.
- **Thread safety:** `notifySENDProgress` runs on the same worker thread as `sendSTORERequest` (a Java-spawned thread already attached to JVM). The `JNIEnv*` and local `jobject` ref are valid for the entire synchronous call. UI updates are marshalled via `runOnUiThread`.
- **Null callback:** If `callback` is null, `ProgressSCU` skips the JNI callback — behaves like the old code.

## Verification

1. Build the project (Gradle will recompile native code via CMake).
2. Select a `.dcm` file in UploadFragment and tap "Upload to PACS".
3. Confirm the horizontal progress bar appears and advances with byte count + percentage in the status text.
4. Confirm "C-STORE Result: Success/Failed" appears on completion and the bar hides.
5. Check `adb logcat -s DcmtkJni` for the existing `native_cStore` debug logs to verify the flow.
