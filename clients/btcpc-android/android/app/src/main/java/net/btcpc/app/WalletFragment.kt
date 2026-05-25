package net.btcpc.app

import android.content.Context
import android.content.SharedPreferences
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.biometric.BiometricManager
import androidx.biometric.BiometricPrompt
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKeys
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import net.btcpc.app.databinding.FragmentWalletBinding
import net.btcpc.uniffi.MobileCoreException
import net.btcpc.uniffi.`derivePostingPubkey`
import net.btcpc.uniffi.`fetchBalance`
import net.btcpc.uniffi.`signTransfer`
import net.btcpc.uniffi.`postTransfer`
import net.btcpc.uniffi.`signStake`
import net.btcpc.uniffi.`postStake`
import net.btcpc.uniffi.`signUnstake`
import net.btcpc.uniffi.`postUnstake`

class WalletFragment : Fragment() {

    private var _binding: FragmentWalletBinding? = null
    private val binding get() = _binding!!

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, saved: Bundle?): View {
        _binding = FragmentWalletBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        val prefs = getSecurePrefs()
        val mnemonic = prefs.getString("mnemonic", null)
        val account  = prefs.getString("account", null)

        if (mnemonic != null && account != null) {
            showWallet(mnemonic, account)
        } else {
            binding.importLayout.visibility = View.VISIBLE
            binding.walletInfo.visibility = View.GONE
        }

        binding.btnImport.setOnClickListener {
            val words = binding.mnemonicInput.text.toString().trim()
            val acc   = binding.accountInput.text.toString().trim()
            if (words.isNotBlank() && acc.isNotBlank()) importWallet(words, acc)
        }

        binding.btnClear.setOnClickListener {
            getSecurePrefs().edit().clear().apply()
            binding.mnemonicInput.setText("")
            binding.accountInput.setText("")
            binding.importLayout.visibility = View.VISIBLE
            binding.walletInfo.visibility = View.GONE
        }

        binding.btnRefresh.setOnClickListener {
            val a = getSecurePrefs().getString("account", null) ?: return@setOnClickListener
            refreshBalance(a)
        }

        binding.btnSend.setOnClickListener { onSendClicked() }
        binding.btnStake.setOnClickListener { onStakeClicked(unstake = false) }
        binding.btnUnstake.setOnClickListener { onStakeClicked(unstake = true) }
    }

    private fun importWallet(words: String, account: String) {
        binding.btnImport.isEnabled = false
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { `derivePostingPubkey`(words) }
            }
            result.onSuccess {
                getSecurePrefs().edit()
                    .putString("mnemonic", words)
                    .putString("account", account)
                    .apply()
                showWallet(words, account)
            }.onFailure { e ->
                binding.btnImport.isEnabled = true
                binding.mnemonicInput.error = "Invalid mnemonic: ${e.message}"
            }
        }
    }

    private fun showWallet(mnemonic: String, account: String) {
        binding.importLayout.visibility = View.GONE
        binding.walletInfo.visibility = View.VISIBLE
        binding.accountText.text = account
        binding.pubkeyText.text = "Deriving…"
        binding.balanceText.text = "—"

        lifecycleScope.launch {
            withContext(Dispatchers.IO) { runCatching { `derivePostingPubkey`(mnemonic) } }
                .onSuccess { binding.pubkeyText.text = it }
                .onFailure { binding.pubkeyText.text = "Error" }
        }

        refreshBalance(account)
    }

    private fun refreshBalance(account: String) {
        val nodeUrl = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"
        binding.balanceText.text = "Fetching…"
        lifecycleScope.launch {
            withContext(Dispatchers.IO) { runCatching { `fetchBalance`(nodeUrl, account) } }
                .onSuccess { json ->
                    val dreams = Regex(""""balance"\s*:\s*(\d+)""").find(json)?.groupValues?.get(1)
                    if (dreams != null) {
                        val btcpc = dreams.toLongOrNull()?.let { it.toDouble() / 100_000_000.0 }
                        binding.balanceText.text = "${btcpc ?: dreams} BTCPC"
                    } else {
                        binding.balanceText.text = json
                    }
                }
                .onFailure { binding.balanceText.text = "Unavailable" }
        }
    }

    private fun onSendClicked() {
        val to     = binding.sendTo.text.toString().trim()
        val amount = binding.sendAmount.text.toString().trim().toDoubleOrNull()
        val memo   = binding.sendMemo.text.toString().trim().ifBlank { null }

        if (to.isEmpty() || amount == null || amount <= 0) {
            showTxResult("Fill in recipient and amount.")
            return
        }

        requireAuth("Confirm send") {
            val prefs    = getSecurePrefs()
            val mnemonic = prefs.getString("mnemonic", null) ?: return@requireAuth
            val account  = prefs.getString("account", null) ?: return@requireAuth
            val nodeUrl  = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"
            val chainId  = settings().getString("chain_id", "btcpc-satoshi") ?: "btcpc-satoshi"
            val dreams   = (amount * 100_000_000.0).toLong().toULong()
            val nonce    = System.currentTimeMillis().toULong()

            binding.btnSend.isEnabled = false
            showTxResult("Signing…")

            lifecycleScope.launch {
                val sig = withContext(Dispatchers.IO) {
                    runCatching { `signTransfer`(mnemonic, chainId, account, to, dreams, "dreams", nonce) }
                }
                if (sig.isFailure) {
                    showTxResult("Sign error: ${sig.exceptionOrNull()?.message}")
                    binding.btnSend.isEnabled = true
                    return@launch
                }
                showTxResult("Submitting…")
                withContext(Dispatchers.IO) {
                    runCatching { `postTransfer`(nodeUrl, account, to, dreams, "dreams", memo, account, nonce, sig.getOrThrow()) }
                }.onSuccess { hash ->
                    showTxResult("Sent!\nHash: $hash")
                }.onFailure { e ->
                    val msg = e.message ?: "Unknown error"
                    showTxResult(if ("not connected" in msg) "Node has no peers — not broadcast." else "Error: $msg")
                }
                binding.btnSend.isEnabled = true
            }
        }
    }

    private fun onStakeClicked(unstake: Boolean) {
        val amount = binding.stakeAmount.text.toString().trim().toDoubleOrNull()
        if (amount == null || amount <= 0) { showTxResult("Enter a valid amount."); return }

        val label = if (unstake) "Confirm unstake" else "Confirm stake"
        requireAuth(label) {
            val prefs    = getSecurePrefs()
            val mnemonic = prefs.getString("mnemonic", null) ?: return@requireAuth
            val account  = prefs.getString("account", null) ?: return@requireAuth
            val nodeUrl  = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"
            val chainId  = settings().getString("chain_id", "btcpc-satoshi") ?: "btcpc-satoshi"
            val dreams   = (amount * 100_000_000.0).toLong().toULong()
            val nonce    = System.currentTimeMillis().toULong()

            binding.btnStake.isEnabled = false
            binding.btnUnstake.isEnabled = false
            showTxResult(if (unstake) "Unstaking…" else "Staking…")

            lifecycleScope.launch {
                val result = withContext(Dispatchers.IO) {
                    runCatching {
                        if (unstake) {
                            val sig = `signUnstake`(mnemonic, chainId, account, dreams, nonce)
                            `postUnstake`(nodeUrl, account, dreams, nonce, account, sig)
                        } else {
                            val sig = `signStake`(mnemonic, chainId, account, dreams, nonce)
                            `postStake`(nodeUrl, account, dreams, nonce, account, sig)
                        }
                    }
                }
                result.onSuccess { hash -> showTxResult("Done!\nHash: $hash") }
                       .onFailure { e   -> showTxResult("Error: ${e.message}") }
                binding.btnStake.isEnabled = true
                binding.btnUnstake.isEnabled = true
            }
        }
    }

    private fun showTxResult(msg: String) {
        binding.txResult.visibility = View.VISIBLE
        binding.txResult.text = msg
    }

    // ── Biometric auth ────────────────────────────────────────────────────────

    private fun requireAuth(title: String, onAuth: () -> Unit) {
        val authEnabled = settings().getBoolean("require_auth", true)
        if (!authEnabled) { onAuth(); return }

        val canAuth = BiometricManager.from(requireContext())
            .canAuthenticate(
                BiometricManager.Authenticators.BIOMETRIC_STRONG or
                BiometricManager.Authenticators.DEVICE_CREDENTIAL
            )

        if (canAuth != BiometricManager.BIOMETRIC_SUCCESS) {
            // No biometrics enrolled — fall through (device may have no screen lock).
            onAuth()
            return
        }

        val executor = ContextCompat.getMainExecutor(requireContext())
        val prompt = BiometricPrompt(this, executor, object : BiometricPrompt.AuthenticationCallback() {
            override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
                onAuth()
            }
            override fun onAuthenticationError(errorCode: Int, errString: CharSequence) {
                showTxResult("Auth cancelled: $errString")
            }
            override fun onAuthenticationFailed() {
                showTxResult("Authentication failed.")
            }
        })

        val promptInfo = BiometricPrompt.PromptInfo.Builder()
            .setTitle(title)
            .setSubtitle("Confirm your identity to sign the transaction")
            .setAllowedAuthenticators(
                BiometricManager.Authenticators.BIOMETRIC_STRONG or
                BiometricManager.Authenticators.DEVICE_CREDENTIAL
            )
            .build()

        prompt.authenticate(promptInfo)
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private fun getSecurePrefs(): SharedPreferences {
        val keyAlias = MasterKeys.getOrCreate(MasterKeys.AES256_GCM_SPEC)
        return EncryptedSharedPreferences.create(
            "btcpc_wallet_secure",
            keyAlias,
            requireContext(),
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

    private fun settings(): SharedPreferences =
        requireActivity().getSharedPreferences("btcpc_settings", Context.MODE_PRIVATE)

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
