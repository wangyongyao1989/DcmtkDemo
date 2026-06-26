package com.example.dcmtkdemo;

public interface ProgressCallback {
    void onProgress(long sent, long total);
}
