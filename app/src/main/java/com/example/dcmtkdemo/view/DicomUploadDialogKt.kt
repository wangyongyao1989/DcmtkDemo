package com.example.dcmtkdemo.view

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.ViewModelProvider
import com.example.dcmtkdemo.R
import com.example.dcmtkdemo.viewmodel.PacsViewModel

class DicomUploadDialogKt : DialogFragment() {

    private var uploadView: DicomUploadViewKt? = null
    private lateinit var viewModel: PacsViewModel
    private var dcmPaths: Array<String>? = null
    private var listener: OnUploadFinishedListener? = null
    private var autoStart = false

    interface OnUploadFinishedListener {
        fun onFinished(successCount: Int, totalCount: Int)
    }

    companion object {
        @JvmStatic
        fun newInstance(paths: Array<String>): DicomUploadDialogKt {
            return newInstance(paths, false)
        }

        @JvmStatic
        fun newInstance(paths: Array<String>, autoStart: Boolean): DicomUploadDialogKt {
            return DicomUploadDialogKt().apply {
                this.dcmPaths = paths
                this.autoStart = autoStart
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
        uploadView = DicomUploadViewKt(requireContext(), dcmPaths).apply {
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
            setOnUploadEventListener(object : DicomUploadViewKt.OnUploadEventListener {
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
