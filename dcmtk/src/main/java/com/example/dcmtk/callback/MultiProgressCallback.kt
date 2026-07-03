package com.example.dcmtk.callback;

public interface MultiProgressCallback {
    /**
     * @return true to continue, false to cancel
     */
    boolean onProgress(int index, long sent, long total);
}
