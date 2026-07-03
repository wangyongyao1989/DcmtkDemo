package com.example.dcmtk.callback

interface ProgressCallback {
    fun onProgress(sent: Long, total: Long)
}
