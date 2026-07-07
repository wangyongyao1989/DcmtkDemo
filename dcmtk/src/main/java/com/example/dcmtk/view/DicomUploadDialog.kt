package com.example.dcmtk.view

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.ViewModelProvider
import com.example.dcmtk.R
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.viewmodel.PacsViewModel

class DicomUploadDialog : DialogFragment() {

    private var uploadView: DicomUploadView? = null
    private lateinit var viewModel: PacsViewModel
    private var dcmPaths: Array<String>? = null
    private var listener: OnUploadFinishedListener? = null
    private var autoStart = false

    private var pacsConfig: PacsConfig? = null

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
            config: PacsConfig?
        ): DicomUploadDialog {
            return DicomUploadDialog().apply {
                this.dcmPaths = paths
                this.autoStart = autoStart
                this.pacsConfig = config
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
            return newInstance(paths, autoStart, PacsConfig(host ?: ""
                , port, localAet ?: "", remoteAet ?: ""))
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
        viewModel = ViewModelProvider(requireActivity()).get(PacsViewModel::class.java)
        uploadView = DicomUploadView(requireContext(), pacsConfig ?: viewModel.pacsConfig.value, dcmPaths).apply {
            setBackgroundResource(R.drawable.popup_bg)
        }
        return uploadView
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        uploadView?.apply {
            setOnUploadEventListener(object : DicomUploadView.OnUploadEventListener {
                override fun onFinished(successCount: Int, totalCount: Int) {
                    listener?.onFinished(successCount, totalCount)
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
                val width = (resources.displayMetrics.widthPixels * 0.55).toInt()
                setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT)
                setBackgroundDrawableResource(android.R.color.transparent)
            }
        }
    }
}
