package com.example.dcmtk.view

import android.annotation.SuppressLint
import android.content.Context
import android.util.AttributeSet
import android.view.LayoutInflater
import android.view.View
import androidx.constraintlayout.widget.ConstraintLayout
import com.example.dcmtk.PacsManager
import com.example.dcmtk.R
import com.example.dcmtk.callback.MultiProgressCallback
import com.example.dcmtk.databinding.ViewDicomUploadV2Binding
import com.example.dcmtk.model.DicomImageRecord
import com.example.dcmtk.model.PacsConfig
import kotlinx.coroutines.*
import java.io.File
import java.util.*
import java.util.concurrent.atomic.AtomicBoolean

class DicomUploadViewV2 @JvmOverloads constructor(
    context: Context,
    private var pacsConfig: PacsConfig? = null,
    private var records: Array<DicomImageRecord>? = null,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : ConstraintLayout(context, attrs, defStyleAttr) {

    private val binding: ViewDicomUploadV2Binding =
        ViewDicomUploadV2Binding.inflate(LayoutInflater.from(context)
            , this, true)
    private val isCancelled = AtomicBoolean(false)
    private var isUploading = false
    private var isFinished = false
    private var listener: OnUploadEventListener? = null
    
    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())

    interface OnUploadEventListener {
        fun onFinished(successCount: Int, totalCount: Int)
        fun onCancel()
    }

    init {
        initView()
    }

    private fun initView() {
        binding.btnExit.setOnClickListener {
            if (isUploading) {
                isCancelled.set(true)
                binding.btnExit.isEnabled = false
                listener?.onCancel()
            } else {
                listener?.onCancel()
            }
        }
        
        records?.let {
            val total = it.size
            it.firstOrNull()?.let { first ->
                binding.tvCurrentProgress.text = context.getString(
                    R.string.dicom_upload_current_item, 1, total, first.name
                )
            }
        }
    }


    fun setOnUploadEventListener(listener: OnUploadEventListener?) {
        this.listener = listener
    }

    fun startUploadProcess() {
        val currentRecords = records
        val config = pacsConfig
        if (currentRecords.isNullOrEmpty() || config == null) {
            return
        }
        isUploading = true
        val paths = currentRecords.map { it.dcmPath }.toTypedArray()
        val totalFiles = paths.size

        scope.launch {
            val echoOk = PacsManager.safeCEcho(config, 1)
            
            if (!echoOk) {
                withContext(Dispatchers.Main) {
                    isUploading = false
                    // Handle error if needed
                }
                return@launch
            }

            val successCount = PacsManager.safeCStoreMulti(config, paths
                , object : MultiProgressCallback {
                override fun onProgress(index: Int, sent: Long, total: Long): Boolean {
                    if (isCancelled.get()) return false
                    launch(Dispatchers.Main) {
                        var percent = if (total > 0) (sent * 100 / total).toInt() else 0
                        if (percent > 100) percent = 100
                        binding.progressBar.progress = percent
                        
                        val currentRecord = currentRecords[index]
                        binding.tvCurrentProgress.text = context.getString(
                            R.string.dicom_upload_current_item,
                            index + 1, totalFiles, currentRecord.name
                        )
                        binding.tvFileDetail.text = String.format(
                            Locale.getDefault(), "%s / %s (%d%%)",
                            formatBytes(sent), formatBytes(total), percent
                        )
                    }
                    return true
                }
            }, 1)

            withContext(Dispatchers.Main) {
                isUploading = false
                isFinished = true
                binding.btnExit.isEnabled = true
                binding.tvCurrentProgress.text = context.getString(R.string.dicom_upload_finished)
                val failedCount = totalFiles - successCount
                binding.tvFileDetail.text = context.getString(
                    R.string.dicom_upload_result_summary,
                    successCount, failedCount, totalFiles
                )
                listener?.onFinished(successCount, totalFiles)
            }
        }
    }

    @SuppressLint("DefaultLocale")
    private fun formatBytes(bytes: Long): String {
        return when {
            bytes < 1024 -> "$bytes B"
            bytes < 1024 * 1024 -> String.format("%.1f KB", bytes / 1024.0)
            else -> String.format("%.2f MB", bytes / (1024.0 * 1024.0))
        }
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        scope.cancel()
    }
}
