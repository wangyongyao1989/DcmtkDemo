package com.example.dcmtk.view

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.ViewModelProvider
import com.example.dcmtk.R
import com.example.dcmtk.viewmodel.PacsViewModel

class DicomUploadDialog : DialogFragment() {

    private var uploadView: DicomUploadView? = null
    private lateinit var viewModel: PacsViewModel
    private var dcmPaths: Array<String>? = null
    private var listener: OnUploadFinishedListener? = null
    private var autoStart = false

    private var host: String? = null
    private var port: Int = 0
    private var localAet: String? = null
    private var remoteAet: String? = null

    interface OnUploadFinishedListener {
        fun onFinished(successCount: Int, totalCount: Int)
    }

    companion object {
        @JvmStatic
        fun newInstance(paths: Array<String>): DicomUploadDialog {
            return newInstance(paths, false)
        }

        @JvmStatic
        fun newInstance(paths: Array<String>, autoStart: Boolean): DicomUploadDialog {
            return DicomUploadDialog().apply {
                this.dcmPaths = paths
                this.autoStart = autoStart
            }
        }

        @JvmStatic
        fun newInstance(
            paths: Array<String>,
            autoStart: Boolean,
            host: String?,
            port: Int,
            localAet: String?,
            remoteAet: String?
        ): DicomUploadDialog {
            return DicomUploadDialog().apply {
                this.dcmPaths = paths
                this.autoStart = autoStart
                this.host = host
                this.port = port
                this.localAet = localAet
                this.remoteAet = remoteAet
            }
        }
    }

    fun setOnUploadFinishedListener(listener: OnUploadFinishedListener?) {
        this.listener = listener
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        isCancelable = false
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View? {
        uploadView = DicomUploadView(requireContext(), dcmPaths).apply {
            setBackgroundResource(R.drawable.popup_bg)
        }
        return uploadView
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        viewModel = ViewModelProvider(requireActivity()).get(PacsViewModel::class.java)

        uploadView?.apply {
            setConnectionInfo(
                viewModel.host.value,
                viewModel.port.value ?: 0,
                viewModel.localAet.value,
                viewModel.remoteAet.value
            )
            setOnUploadEventListener(object : DicomUploadView.OnUploadEventListener {
                override fun onFinished(successCount: Int, totalCount: Int) {
                    listener?.onFinished(successCount, totalCount)
                    dismiss()
                }

                override fun onCancel() {
                    dismiss()
                }
            })

            if (autoStart) {
                startUploadProcess()
            }
        }
    }

    override fun onStart() {
        super.onStart()
        dialog?.apply {
            setCancelable(false)
            setCanceledOnTouchOutside(false)
            window?.apply {
                val width = (resources.displayMetrics.widthPixels * 0.90).toInt()
                setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT)
                setBackgroundDrawableResource(android.R.color.transparent)
            }
        }
    }
}
