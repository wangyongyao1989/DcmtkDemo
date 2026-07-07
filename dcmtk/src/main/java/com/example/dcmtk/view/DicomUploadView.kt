package com.example.dcmtk.view

import android.annotation.SuppressLint
import android.content.Context
import android.util.AttributeSet
import android.view.LayoutInflater
import android.view.View
import androidx.constraintlayout.widget.ConstraintLayout
import com.example.dcmtk.PacsManager
import com.example.dcmtk.callback.MultiProgressCallback
import com.example.dcmtk.databinding.ViewDicomUploadBinding
import com.example.dcmtk.model.DicomImageRecord
import com.example.dcmtk.model.PacsConfig
import kotlinx.coroutines.*
import java.io.File
import java.util.*
import java.util.concurrent.atomic.AtomicBoolean

class DicomUploadView @JvmOverloads constructor(
    context: Context,
    private var pacsConfig: PacsConfig? = null,
    private var dcmPaths: Array<String>? = null,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : ConstraintLayout(context, attrs, defStyleAttr) {

    private val binding: ViewDicomUploadBinding =
        ViewDicomUploadBinding.inflate(LayoutInflater.from(context)

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
        binding.btnCancel.setOnClickListener {
            if (isUploading) {
                isCancelled.set(true)
                binding.tvStatus.text = "Cancelling..."
                binding.btnCancel.isEnabled = false
                listener?.onCancel()
            } else if (isFinished) {
                listener?.onCancel()
            } else {
                listener?.onCancel()
            }
        }
    }


    fun setOnUploadEventListener(listener: OnUploadEventListener?) {
        this.listener = listener
    }

    fun setConnectionInfo(config: PacsConfig?) {
        this.pacsConfig = config
    }

    fun setConnectionInfo(host: String?, port: Int, local: String?, remote: String?) {
        setConnectionInfo(PacsConfig(host ?: "", port, local ?: "", remote ?: ""))
    }

    fun startUploadProcess() {
        val paths = dcmPaths
        val config = pacsConfig
        if (paths.isNullOrEmpty()) {
            showError("No records to upload")
            return
        }
        if (config == null) {
            showError("Connection info is missing")
            return
        }
        isUploading = true
        binding.layoutUploadProgress.visibility = View.VISIBLE
        binding.tvTitle.text = "Uploading DICOM Files"

        scope.launch {
            val echoOk = PacsManager.safeCEcho(config, 1)
            
            if (!echoOk) {
                withContext(Dispatchers.Main) {
                    showError("PACS Verification Failed (Check Network/AETs)")
                }
                return@launch
            }

            val totalFiles = paths.size

            val successCount = PacsManager.safeCStoreMulti(config, paths
                , object : MultiProgressCallback {
                override fun onProgress(index: Int, sent: Long, total: Long): Boolean {
                    if (isCancelled.get()) return false
                    launch(Dispatchers.Main) {
                        var percent = if (total > 0) (sent * 100 / total).toInt() else 0
                        if (percent > 100) percent = 100
                        binding.progressBar.progress = percent
                        binding.tvStatus.text = String.format(
                            Locale.getDefault(), "Uploading (%d/%d): %s\n%s / %s (%d%%)",
                            index + 1, totalFiles, File(paths[index]).name,
                            formatBytes(sent), formatBytes(total), percent
                        )
                    }
                    return true
                }
            }, 1)

            withContext(Dispatchers.Main) {
                isUploading = false
                isFinished = true
                binding.btnCancel.isEnabled = true
                binding.btnCancel.text = "确定"
                binding.tvStatus.text = "Upload Finished: $successCount/$totalFiles successful"
                listener?.onFinished(successCount, totalFiles)
            }
        }
    }

    private fun showError(message: String) {
        isUploading = false
        binding.layoutUploadProgress.visibility = View.GONE
        binding.tvTitle.text = "DICOM Upload"
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
