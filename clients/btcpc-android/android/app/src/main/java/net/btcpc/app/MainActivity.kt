package net.btcpc.app

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.fragment.app.Fragment
import net.btcpc.app.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    private val walletFragment   by lazy { WalletFragment() }
    private val nodeFragment     by lazy { NodeFragment() }
    private val flipperFragment  by lazy { FlipperFragment() }
    private val settingsFragment by lazy { SettingsFragment() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        if (savedInstanceState == null) {
            replaceFragment(walletFragment)
        }

        binding.bottomNav.setOnItemSelectedListener { item ->
            when (item.itemId) {
                R.id.nav_wallet   -> { replaceFragment(walletFragment);   true }
                R.id.nav_node     -> { replaceFragment(nodeFragment);     true }
                R.id.nav_flipper  -> { replaceFragment(flipperFragment);  true }
                R.id.nav_settings -> { replaceFragment(settingsFragment); true }
                else -> false
            }
        }
    }

    private fun replaceFragment(fragment: Fragment) {
        supportFragmentManager.beginTransaction()
            .replace(R.id.fragment_container, fragment)
            .commit()
    }
}
