package com.example.dcmtkdemo.view

import android.annotation.SuppressLint
import android.content.Context
import android.util.AttributeSet
import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import com.example.dcmtk.PacsManager
import com.example.dcmtk.callback.MultiProgressCallback
import com.example.dcmtkdemo.databinding.ViewDicomUploadBinding
import com.example.dcmtkdemo.model.DicomImageRecord
import kotlinx.coroutines.*
import java.io.File
import java.util.*
import java.util.concurrent.atomic.AtomicBoolean

class DicomUploadViewKt @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : LinearLayout(context, attrs, defStyleAttr) {

    private val binding: ViewDicomUploadBinding =
        ViewDicomUploadBinding.inflate(LayoutInflater.from(context), this, true)
    private var uploadRecords: List<DicomImageRecord>? = null
    private val isCancelled = AtomicBoolean(false)
    private var isUploading = false
    private var listener: OnUploadEventListener? = null
    
    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())

    interface OnUploadEventListener {
        fun onFinished(successCount: Int, totalCount: Int)
        fun onCancel()
    }

    init {
        orientation = VERTICAL
        initView()
    }

    private fun initView() {
        binding.btnAction.setOnClickListener {
            if (!isUploading) {
                startUploadProcess()
            }
        }

        binding.btnCancel.setOnClickListener {
            if (isUploading) {
                isCancelled.set(true)
                binding.tvStatus.text = "Cancelling..."
                binding.btnCancel.isEnabled = false
            } else {
                listener?.onCancel()
            }
        }
    }

    fun setUploadRecords(records: List<DicomImageRecord>?) {
        this.uploadRecords = records
    }

    fun setOnUploadEventListener(listener: OnUploadEventListener?) {
        this.listener = listener
    }

    fun setConnectionInfo(host: String?, port: Int, local: String?, remote: String?) {
        (binding.connectionView as? PacsConnectionViewKt)?.setConnectionInfo(host, port, local, remote)
    }

    fun startUploadProcess() {
        val records = uploadRecords
        if (records.isNullOrEmpty()) {
            showError("No records to upload")
            return
        }

        val connView = binding.connectionView as? PacsConnectionViewKt
        if (connView == null) {
            showError("Invalid Connection View")
            return
        }

        val host = connView.getHost()
        val port = connView.getPort()
        val local = connView.getLocalAet()
        val remote = connView.getRemoteAet()

        if (host.isEmpty() || port == 0 || local.isEmpty() || remote.isEmpty()) {
            showError("Please fill all fields")
            return
        }

        binding.tvError.visibility = GONE
        isUploading = true
        binding.connectionView.visibility = View.GONE
        binding.layoutUploadProgress.visibility = View.VISIBLE
        binding.btnAction.visibility = View.GONE
        binding.tvTitle.text = "Uploading DICOM Files"

        scope.launch {
            val echoOk = withContext(Dispatchers.IO) {
                PacsManager.safeCEchoSync(host, port, local, remote, 1)
            }
            
            if (!echoOk) {
                showError("PACS Verification Failed (Check Network/AETs)")
                return@launch
            }

            val totalFiles = records.size
            val paths = records.map { it.dcmPath }.toTypedArray()

            val successCount = withContext(Dispatchers.IO) {
                PacsManager.safeCStoreMultiSync(host, port, local, remote, paths, object : MultiProgressCallback {
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
            }

            listener?.onFinished(successCount, totalFiles)
        }
    }

    private fun showError(message: String) {
        binding.tvError.text = message
        binding.tvError.visibility = VISIBLE
        isUploading = false
        binding.connectionView.visibility = View.VISIBLE
        binding.layoutUploadProgress.visibility = View.GONE
        binding.btnAction.visibility = View.VISIBLE
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
