package com.example.dcmtkdemo.callback;

public interface ProgressCallback {
    void onProgress(long sent, long total);
}
