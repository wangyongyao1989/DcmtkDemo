package com.example.dcmtk.callback

interface MultiProgressCallback {
    /**
     * @return true to continue, false to cancel
     */
    fun onProgress(index: Int, sent: Long, total: Long): Boolean
}
