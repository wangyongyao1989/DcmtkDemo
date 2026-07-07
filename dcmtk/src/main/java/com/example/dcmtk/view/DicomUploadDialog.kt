package com.example.dcmtk.view

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.ViewModelProvider
import com.example.dcmtk.PacsManager
import com.example.dcmtk.model.DicomImageRecord
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.viewmodel.PacsViewModel

class DicomUploadDialog : DialogFragment() {

    private var uploadView: DicomUploadView? = null
    private lateinit var viewModel: PacsViewModel
    private var records: Array<DicomImageRecord>? = null
    private var listener: OnUploadFinishedListener? = null
    private var autoStart = false

    private var pacsConfig: PacsConfig? = null

    interface OnUploadFinishedListener {
        fun onFinished(successCount: Int, totalCount: Int)
    }

    companion object {
        @JvmStatic
        fun newInstance(records: Array<DicomImageRecord>): DicomUploadDialog {
            return newInstance(records, false)
        }

        @JvmStatic
        fun newInstance(records: Array<DicomImageRecord>, autoStart: Boolean): DicomUploadDialog {
            return DicomUploadDialog().apply {
                this.records = records
                this.autoStart = autoStart
            }
        }

        @JvmStatic
        fun newInstance(
            records: Array<DicomImageRecord>,
            autoStart: Boolean,
            config: PacsConfig?
        ): DicomUploadDialog {
            return DicomUploadDialog().apply {
                this.records = records
                this.autoStart = autoStart
                this.pacsConfig = config
            }
        }

        @JvmStatic
        fun newInstance(
            records: Array<DicomImageRecord>,
            autoStart: Boolean,
            host: String?,
            port: Int,
            localAet: String?,
            remoteAet: String?
        ): DicomUploadDialog {
            return newInstance(records, autoStart, PacsConfig(host ?: ""
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
        uploadView = DicomUploadView(requireContext(), pacsConfig ?: viewModel.pacsConfig.value
            , records).apply {
            // Background is already set in XML for the new view
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
                    PacsManager.cancelOperation()
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
                val width = (resources.displayMetrics.widthPixels * 0.45).toInt()
                setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT)
                setBackgroundDrawableResource(android.R.color.transparent)
            }
        }
    }
}
