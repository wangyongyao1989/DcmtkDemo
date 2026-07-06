package com.example.dcmtk.view

import android.content.Context
import android.util.AttributeSet
import android.view.LayoutInflater
import android.view.View
import android.widget.Toast
import androidx.constraintlayout.widget.ConstraintLayout
import com.example.dcmtk.PacsManager
import com.example.dcmtk.R
import com.example.dcmtk.databinding.ViewPacsConnectionBinding
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.utils.PacsPrefs
import kotlinx.coroutines.*

class PacsConnectionView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : ConstraintLayout(context, attrs, defStyleAttr) {

    private val binding: ViewPacsConnectionBinding =
        ViewPacsConnectionBinding.inflate(LayoutInflater.from(context), this)
    private var listener: OnConnectionVerifiedListener? = null
    
    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())

    interface OnConnectionVerifiedListener {
        fun onVerified(config: PacsConfig)
        fun onCancel()
    }

    init {
        val p = (20 * resources.displayMetrics.density).toInt()
        setPadding(p, p, p, p)
        setBackgroundResource(R.drawable.connection_dialog_bg)

        initView()
        // 自动填充本地保存的配置
        loadSavedConfig()
    }

    private fun initView() {
        binding.btnVerify.setOnClickListener {
            val config = getConfig() ?: return@setOnClickListener

            binding.tvError.visibility = View.GONE
            binding.btnVerify.isEnabled = false
            binding.btnCancel.isEnabled = false

            scope.launch {
                val echoSuccess = PacsManager.safeCEcho(config, 2)
                
                if (echoSuccess) {
                    binding.tvError.visibility = View.GONE
                    Toast.makeText(context, "Connection & C-ECHO Verified", Toast.LENGTH_SHORT).show()
                    // 验证成功后保存到 SP
                    PacsPrefs.saveConfig(context, config)
                    listener?.onVerified(config)
                } else {
                    showError("Verification Failed (Check Network or AETs)")
                }
                binding.btnVerify.isEnabled = true
                binding.btnCancel.isEnabled = true
            }
        }

        binding.btnCancel.setOnClickListener {
            // 取消正在进行的验证协程
            scope.coroutineContext.cancelChildren()
            listener?.onCancel()
        }
    }

    private fun loadSavedConfig() {
        val savedConfig = PacsPrefs.getConfig(context)
        setConnectionInfo(savedConfig)
    }

    fun setOnConnectionVerifiedListener(listener: OnConnectionVerifiedListener?) {
        this.listener = listener
    }

    private fun showError(message: String) {
        binding.tvError.text = message
        binding.tvError.visibility = View.VISIBLE
    }

    fun setConnectionInfo(config: PacsConfig?) {
        binding.etHost.setText(config?.host ?: "")
        binding.etPort.setText(config?.port?.toString() ?: "11112")
        binding.etLocalAet.setText(config?.localAet ?: "")
        binding.etRemoteAet.setText(config?.remoteAet ?: "")
    }

    fun setConnectionInfo(host: String?, port: Int, local: String?, remote: String?) {
        setConnectionInfo(PacsConfig(host ?: "", port, local ?: "", remote ?: ""))
    }

    fun getConfig(): PacsConfig? {
        val host = binding.etHost.text.toString().trim()
        val portStr = binding.etPort.text.toString().trim()
        val local = binding.etLocalAet.text.toString().trim()
        val remote = binding.etRemoteAet.text.toString().trim()

        if (host.isEmpty() || portStr.isEmpty() || local.isEmpty() || remote.isEmpty()) {
            showError("Please fill all fields")
            return null
        }

        val port = portStr.toIntOrNull() ?: 0
        if (port <= 0) {
            showError("Invalid port number")
            return null
        }

        return PacsConfig(host, port, local, remote)
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        scope.cancel()
    }
}
