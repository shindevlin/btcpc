package net.btcpc.app

import android.content.Context
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.fragment.app.Fragment
import net.btcpc.app.databinding.FragmentSettingsBinding

class SettingsFragment : Fragment() {

    private var _binding: FragmentSettingsBinding? = null
    private val binding get() = _binding!!

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, saved: Bundle?): View {
        _binding = FragmentSettingsBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        val prefs = requireActivity().getSharedPreferences("btcpc_settings", Context.MODE_PRIVATE)

        binding.nodeUrlInput.setText(
            prefs.getString("node_url", "http://10.0.2.2:4242")
        )
        binding.chainIdInput.setText(
            prefs.getString("chain_id", "btcpc-satoshi")
        )
        binding.switchAuth.isChecked = prefs.getBoolean("require_auth", true)

        binding.btnSave.setOnClickListener {
            val url     = binding.nodeUrlInput.text.toString().trim()
            val chainId = binding.chainIdInput.text.toString().trim()
            val auth    = binding.switchAuth.isChecked

            if (url.isEmpty()) { binding.nodeUrlInput.error = "Required"; return@setOnClickListener }

            prefs.edit()
                .putString("node_url", url)
                .putString("chain_id", chainId.ifBlank { "btcpc-satoshi" })
                .putBoolean("require_auth", auth)
                .apply()

            Toast.makeText(requireContext(), "Saved", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
