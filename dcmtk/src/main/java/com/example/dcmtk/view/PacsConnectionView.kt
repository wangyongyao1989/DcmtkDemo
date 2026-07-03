package com.example.dcmtk.view

import android.content.Context
import android.util.AttributeSet
import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import android.widget.Toast
import com.example.dcmtk.PacsManager
import com.example.dcmtk.databinding.ViewPacsConnectionBinding
import kotlinx.coroutines.*

class PacsConnectionView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : LinearLayout(context, attrs, defStyleAttr) {

    private val binding: ViewPacsConnectionBinding =
        ViewPacsConnectionBinding.inflate(LayoutInflater.from(context), this)
    private var listener: OnConnectionVerifiedListener? = null
    
    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())

    interface OnConnectionVerifiedListener {
        fun onVerified(host: String, port: Int, local: String, remote: String)
        fun onCancel()
    }

    init {
        orientation = VERTICAL
        initView()
    }

    private fun initView() {
        binding.btnVerify.setOnClickListener {
            val host = getHost()
            val port = getPort()
            val local = getLocalAet()
            val remote = getRemoteAet()

            if (host.isEmpty() || port == 0 || local.isEmpty() || remote.isEmpty()) {
                showError("Please fill all fields")
                return@setOnClickListener
            }

            binding.tvError.visibility = View.GONE
            binding.btnVerify.isEnabled = false
            binding.btnCancel.isEnabled = false

            scope.launch {
                val echoSuccess = withContext(Dispatchers.IO) {
                    PacsManager.safeCEchoSync(host, port, local, remote, 2)
                }
                
                if (echoSuccess) {
                    binding.tvError.visibility = View.GONE
                    Toast.makeText(context, "Connection & C-ECHO Verified", Toast.LENGTH_SHORT).show()
                    listener?.onVerified(host, port, local, remote)
                } else {
                    showError("Verification Failed (Check Network or AETs)")
                }
                binding.btnVerify.isEnabled = true
                binding.btnCancel.isEnabled = true
            }
        }

        binding.btnCancel.setOnClickListener {
            listener?.onCancel()
        }
    }

    fun setOnConnectionVerifiedListener(listener: OnConnectionVerifiedListener?) {
        this.listener = listener
    }

    private fun showError(message: String) {
        binding.tvError.text = message
        binding.tvError.visibility = View.VISIBLE
    }

    fun setConnectionInfo(host: String?, port: Int, local: String?, remote: String?) {
        binding.etHost.setText(host ?: "")
        binding.etPort.setText(port.toString())
        binding.etLocalAet.setText(local ?: "")
        binding.etRemoteAet.setText(remote ?: "")
    }

    fun getHost(): String = binding.etHost.text.toString()
    
    fun getPort(): Int {
        return try {
            binding.etPort.text.toString().toInt()
        } catch (e: Exception) {
            0
        }
    }
    
    fun getLocalAet(): String = binding.etLocalAet.text.toString()
    fun getRemoteAet(): String = binding.etRemoteAet.text.toString()

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        scope.cancel()
    }
}
