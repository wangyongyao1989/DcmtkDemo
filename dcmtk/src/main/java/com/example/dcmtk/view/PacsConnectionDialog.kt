package com.example.dcmtk.view

import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.os.Bundle
import android.view.ViewGroup
import android.view.Window
import com.example.dcmtk.R

class PacsConnectionDialog(
    context: Context,
    private val host: String?,
    private val port: Int?,
    private val localAet: String?,
    private val remoteAet: String?,
    private val listener: PacsConnectionView.OnConnectionVerifiedListener?
) : Dialog(context) {

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

        connectionView.setConnectionInfo(host, port ?: 0, localAet, remoteAet)

        connectionView.setOnConnectionVerifiedListener(object : PacsConnectionView.OnConnectionVerifiedListener {
            override fun onVerified(host: String, port: Int, local: String, remote: String) {
                listener?.onVerified(host, port, local, remote)
                dismiss()
            }

            override fun onCancel() {
                listener?.onCancel()
                dismiss()
            }
        })
    }
}
