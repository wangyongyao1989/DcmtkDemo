package com.example.dcmtk.view

import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.os.Bundle
import android.view.ViewGroup
import android.view.Window
import com.example.dcmtk.R
import com.example.dcmtk.model.PacsConfig

class PacsConnectionDialog(
    context: Context,
    private val config: PacsConfig?,
    private val listener: PacsConnectionView.OnConnectionVerifiedListener?
) : Dialog(context) {

    // Keep the old constructor for compatibility or just update it
    constructor(
        context: Context,
        host: String?,
        port: Int?,
        localAet: String?,
        remoteAet: String?,
        listener: PacsConnectionView.OnConnectionVerifiedListener?
    ) : this(context, PacsConfig(host ?: "", port ?: 0, localAet ?: "", remoteAet ?: ""), listener)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        requestWindowFeature(Window.FEATURE_NO_TITLE)

        val connectionView = PacsConnectionView(context).apply {
            setBackgroundResource(R.drawable.popup_bg)
            setPadding(40, 40, 40, 40)
        }

        setContentView(connectionView)

        setCancelable(false)
        setCanceledOnTouchOutside(false)

        window?.apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            val width = (context.resources.displayMetrics.widthPixels * 0.85).toInt()
            setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT)
        }

        connectionView.setConnectionInfo(config)

        connectionView.setOnConnectionVerifiedListener(object : PacsConnectionView.OnConnectionVerifiedListener {
            override fun onVerified(config: PacsConfig) {
                listener?.onVerified(config)
                dismiss()
            }

            override fun onCancel() {
                listener?.onCancel()
                dismiss()
            }
        })
    }
}
