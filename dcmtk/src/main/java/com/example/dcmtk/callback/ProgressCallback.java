package com.example.dcmtk.callback;

public interface ProgressCallback {
    void onProgress(long sent, long total);
}
