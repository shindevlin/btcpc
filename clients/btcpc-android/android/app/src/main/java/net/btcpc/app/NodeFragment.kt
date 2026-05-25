package net.btcpc.app

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.telephony.TelephonyManager
import android.telephony.CellInfoLte
import android.telephony.CellInfoGsm
import android.telephony.CellInfoWcdma
import android.telephony.CellInfo
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKeys
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import net.btcpc.app.databinding.FragmentNodeBinding
import net.btcpc.uniffi.`fetchInferenceJobs`
import net.btcpc.uniffi.`signInferenceBid`
import net.btcpc.uniffi.`postInferenceBid`
import net.btcpc.uniffi.`signNodeCapability`
import net.btcpc.uniffi.`postNodeCapability`
import net.btcpc.uniffi.`signCoverageReport`
import net.btcpc.uniffi.`postCoverageReport`
import net.btcpc.uniffi.`signSensorCommit`
import net.btcpc.uniffi.`postSensorCommit`
import net.btcpc.uniffi.`postSensorRegister`
import net.btcpc.uniffi.`parseBleFrame`
import net.btcpc.uniffi.`verifyBleFrameSig`
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.security.MessageDigest

class NodeFragment : Fragment() {

    private var _binding: FragmentNodeBinding? = null
    private val binding get() = _binding!!

    // Buffered Flipper frames waiting to be relayed
    private val flipperFrameBuffer = mutableListOf<JSONObject>()

    // Last collected batch JSON for sensor commit
    private var lastBatchJson: String? = null
    private var lastBatchHash: String? = null
    private var lastBatchCount: Int = 0

    // Runtime sensor readings (single sample, updated via SensorEventListener)
    private val latestReadings = mutableMapOf<String, Any>()
    private var sensorManager: SensorManager? = null
    private val sensorListener = object : SensorEventListener {
        override fun onSensorChanged(e: SensorEvent) {
            when (e.sensor.type) {
                Sensor.TYPE_ACCELEROMETER      -> latestReadings["accel"]   = e.values.toList()
                Sensor.TYPE_GYROSCOPE          -> latestReadings["gyro"]    = e.values.toList()
                Sensor.TYPE_MAGNETIC_FIELD     -> latestReadings["mag"]     = e.values.toList()
                Sensor.TYPE_PRESSURE           -> latestReadings["baro_hpa"] = e.values[0]
                Sensor.TYPE_AMBIENT_TEMPERATURE-> latestReadings["temp_c"]  = e.values[0]
                Sensor.TYPE_LIGHT              -> latestReadings["lux"]     = e.values[0]
                Sensor.TYPE_RELATIVE_HUMIDITY  -> latestReadings["humidity"]= e.values[0]
                Sensor.TYPE_PROXIMITY          -> latestReadings["proximity"]= e.values[0]
                Sensor.TYPE_STEP_COUNTER       -> latestReadings["steps"]   = e.values[0]
            }
        }
        override fun onAccuracyChanged(s: Sensor, accuracy: Int) {}
    }

    // Last GPS fix
    private var lastLocation: Location? = null

    // Permission launcher
    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        val ok = grants.values.all { it }
        if (ok) log("Permissions granted.")
        else log("Some permissions denied — sensor coverage may be partial.")
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, saved: Bundle?): View {
        _binding = FragmentNodeBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        val account = getSecurePrefs().getString("account", null)
        binding.nodeAccountText.text = account ?: "No wallet"

        detectAndDisplaySensors()
        registerPhysicalSensors()

        binding.btnCollectCoverage.setOnClickListener { collectAndSubmitCoverage() }
        binding.btnCollectBatch.setOnClickListener { collectBatch() }
        binding.btnSubmitBatch.setOnClickListener { submitBatch() }
        binding.btnRelayFlipper.setOnClickListener { relayFlipperData() }
        binding.btnAnnounce.setOnClickListener { announceCapabilities() }
        binding.btnFetchJobs.setOnClickListener { fetchJobs() }
    }

    // ── Sensor detection + display ────────────────────────────────────────────

    private fun detectAndDisplaySensors() {
        val sm = requireContext().getSystemService(Context.SENSOR_SERVICE) as SensorManager
        val pm = requireContext().packageManager

        val lines = mutableListOf<String>()
        fun check(type: Int, name: String) {
            lines += if (sm.getDefaultSensor(type) != null) "✓ $name" else "✗ $name"
        }

        check(Sensor.TYPE_ACCELEROMETER,       "Accelerometer")
        check(Sensor.TYPE_GYROSCOPE,           "Gyroscope")
        check(Sensor.TYPE_MAGNETIC_FIELD,      "Magnetometer")
        check(Sensor.TYPE_PRESSURE,            "Barometer")
        check(Sensor.TYPE_AMBIENT_TEMPERATURE, "Temperature")
        check(Sensor.TYPE_LIGHT,               "Ambient Light")
        check(Sensor.TYPE_RELATIVE_HUMIDITY,   "Humidity")
        check(Sensor.TYPE_PROXIMITY,           "Proximity")
        check(Sensor.TYPE_STEP_COUNTER,        "Step Counter")

        lines += if (pm.hasSystemFeature(PackageManager.FEATURE_LOCATION_GPS)) "✓ GPS" else "✗ GPS"
        lines += if (pm.hasSystemFeature(PackageManager.FEATURE_TELEPHONY)) "✓ Cell/LTE" else "✗ Cell"
        lines += if (pm.hasSystemFeature(PackageManager.FEATURE_WIFI)) "✓ WiFi scan" else "✗ WiFi"
        lines += if (pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)) "✓ Camera" else "✗ Camera"
        lines += if (pm.hasSystemFeature(PackageManager.FEATURE_MICROPHONE)) "✓ Microphone" else "✗ Mic"
        lines += if (pm.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) "✓ BLE" else "✗ BLE"

        binding.sensorListText.text = lines.joinToString("\n")
    }

    private fun registerPhysicalSensors() {
        val sm = requireContext().getSystemService(Context.SENSOR_SERVICE) as SensorManager
        sensorManager = sm
        val types = listOf(
            Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_GYROSCOPE, Sensor.TYPE_MAGNETIC_FIELD,
            Sensor.TYPE_PRESSURE, Sensor.TYPE_AMBIENT_TEMPERATURE, Sensor.TYPE_LIGHT,
            Sensor.TYPE_RELATIVE_HUMIDITY, Sensor.TYPE_PROXIMITY, Sensor.TYPE_STEP_COUNTER
        )
        for (type in types) {
            sm.getDefaultSensor(type)?.let {
                sm.registerListener(sensorListener, it, SensorManager.SENSOR_DELAY_NORMAL)
            }
        }
    }

    // ── Coverage (GPS + cell) ─────────────────────────────────────────────────

    private fun collectAndSubmitCoverage() {
        val needed = mutableListOf<String>()
        if (!hasPermission(Manifest.permission.ACCESS_FINE_LOCATION)) needed += Manifest.permission.ACCESS_FINE_LOCATION
        if (!hasPermission(Manifest.permission.READ_PHONE_STATE)) needed += Manifest.permission.READ_PHONE_STATE
        if (needed.isNotEmpty()) { permLauncher.launch(needed.toTypedArray()); return }

        val account = getSecurePrefs().getString("account", null) ?: run { log("Import wallet first."); return }
        val mnemonic = getSecurePrefs().getString("mnemonic", null) ?: return

        binding.btnCollectCoverage.isEnabled = false
        binding.coverageStatus.text = "Collecting..."
        log("Reading GPS + cell towers...")

        lifecycleScope.launch {
            val coverage = withContext(Dispatchers.IO) { readCoverage() }
            if (coverage == null) {
                binding.coverageStatus.text = "No GPS fix or cell data."
                binding.btnCollectCoverage.isEnabled = true
                return@launch
            }

            binding.coverageStatus.text = buildString {
                append("${coverage.technology}  ${coverage.mccMnc}\n")
                append("lat=${coverage.lat?.let { "%.5f".format(it) } ?: "?"}")
                append("  lon=${coverage.lon?.let { "%.5f".format(it) } ?: "?"}\n")
                append("signal=${coverage.signalDbm ?: "?"}dBm")
            }

            val nodeUrl = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"
            val chainId = settings().getString("chain_id", "btcpc-satoshi") ?: "btcpc-satoshi"
            val lat = coverage.lat ?: 0.0
            val lon = coverage.lon ?: 0.0

            withContext(Dispatchers.IO) {
                runCatching {
                    val sig = `signCoverageReport`(mnemonic, chainId, account, lat, lon, coverage.mccMnc, coverage.dataHash)
                    `postCoverageReport`(nodeUrl, account, lat, lon, coverage.signalDbm, coverage.mccMnc, coverage.technology, coverage.dataHash, sig)
                }
            }.onSuccess { log("Coverage submitted: $it") }
             .onFailure { log("Coverage error: ${it.message}") }

            binding.btnCollectCoverage.isEnabled = true
        }
    }

    data class CoverageSnapshot(
        val lat: Double?,
        val lon: Double?,
        val signalDbm: Int?,
        val mccMnc: String,
        val technology: String,
        val dataHash: String,
    )

    @SuppressLint("MissingPermission")
    private fun readCoverage(): CoverageSnapshot? {
        val telMgr = requireContext().getSystemService(Context.TELEPHONY_SERVICE) as TelephonyManager

        var signalDbm: Int? = null
        var mccMnc = "000-000"
        var technology = "unknown"

        try {
            @Suppress("DEPRECATION")
            val cells: List<CellInfo> = telMgr.allCellInfo ?: emptyList()

            cells.firstOrNull { it.isRegistered }?.let { cell ->
                @Suppress("DEPRECATION")
                when (cell) {
                    is CellInfoLte -> {
                        val id = cell.cellIdentity as android.telephony.CellIdentityLte
                        mccMnc = "${id.mcc}-${id.mnc}"
                        signalDbm = cell.cellSignalStrength.dbm
                        technology = "LTE"
                    }
                    is CellInfoGsm -> {
                        val id = cell.cellIdentity as android.telephony.CellIdentityGsm
                        mccMnc = "${id.mcc}-${id.mnc}"
                        signalDbm = cell.cellSignalStrength.dbm
                        technology = "GSM"
                    }
                    is CellInfoWcdma -> {
                        val id = cell.cellIdentity as android.telephony.CellIdentityWcdma
                        mccMnc = "${id.mcc}-${id.mnc}"
                        signalDbm = cell.cellSignalStrength.dbm
                        technology = "UMTS"
                    }
                    else -> {}
                }
            }
        } catch (_: Exception) {}

        val loc = getLastKnownGps()

        // Build raw JSON for hashing
        val raw = JSONObject().apply {
            put("ts", System.currentTimeMillis())
            put("mcc_mnc", mccMnc)
            put("tech", technology)
            put("signal_dbm", signalDbm ?: 0)
            put("lat", loc?.latitude ?: 0.0)
            put("lon", loc?.longitude ?: 0.0)
        }
        val dataHash = sha256(raw.toString())

        return CoverageSnapshot(
            lat = loc?.latitude,
            lon = loc?.longitude,
            signalDbm = signalDbm,
            mccMnc = mccMnc,
            technology = technology,
            dataHash = dataHash,
        )
    }

    @SuppressLint("MissingPermission")
    private fun getLastKnownGps(): Location? {
        if (!hasPermission(Manifest.permission.ACCESS_FINE_LOCATION)) return lastLocation
        val lm = requireContext().getSystemService(Context.LOCATION_SERVICE) as LocationManager
        return lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)
            ?: lm.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)
            ?: lastLocation
    }

    // ── Sensor batch (all phone sensors) ─────────────────────────────────────

    @SuppressLint("MissingPermission")
    private fun collectBatch() {
        // Give sensor listener a moment to populate, then snapshot
        binding.btnCollectBatch.isEnabled = false
        binding.batchStatus.text = "Collecting..."

        lifecycleScope.launch {
            // Short delay so SensorEventListener has time to fire at least once
            kotlinx.coroutines.delay(1200)

            val batch = JSONArray()

            // Physical sensor readings
            for ((key, value) in latestReadings) {
                batch.put(JSONObject().apply {
                    put("type", key)
                    put("ts", System.currentTimeMillis())
                    put("value", value.toString())
                })
            }

            // WiFi scan
            if (hasPermission(Manifest.permission.ACCESS_WIFI_STATE)) {
                val wifiMgr = requireContext().applicationContext
                    .getSystemService(Context.WIFI_SERVICE) as WifiManager
                @Suppress("DEPRECATION")
                val results = wifiMgr.scanResults
                val wifiArr = JSONArray()
                for (r in results.take(20)) {
                    wifiArr.put(JSONObject().apply {
                        put("bssid", r.BSSID)
                        put("rssi", r.level)
                        put("freq", r.frequency)
                    })
                }
                if (wifiArr.length() > 0) {
                    batch.put(JSONObject().apply {
                        put("type", "wifi_scan")
                        put("ts", System.currentTimeMillis())
                        put("aps", wifiArr)
                    })
                }
            }

            // GPS
            getLastKnownGps()?.let { loc ->
                batch.put(JSONObject().apply {
                    put("type", "gps")
                    put("ts", loc.time)
                    put("lat", loc.latitude)
                    put("lon", loc.longitude)
                    put("alt", loc.altitude)
                    put("acc", loc.accuracy)
                })
            }

            val count = batch.length()
            val json  = batch.toString()
            val hash  = sha256(json)

            lastBatchJson  = json
            lastBatchHash  = hash
            lastBatchCount = count

            binding.batchStatus.text = "$count readings · hash: ${hash.take(16)}…"
            binding.btnSubmitBatch.isEnabled = true
            binding.btnCollectBatch.isEnabled = true
            log("Batch collected: $count readings")
        }
    }

    private fun submitBatch() {
        val hash  = lastBatchHash ?: run { log("Collect first."); return }
        val count = lastBatchCount
        val mnemonic = getSecurePrefs().getString("mnemonic", null) ?: run { log("Import wallet first."); return }
        val account  = getSecurePrefs().getString("account", null) ?: return
        val nodeUrl  = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"
        val chainId  = settings().getString("chain_id", "btcpc-satoshi") ?: "btcpc-satoshi"

        // sensor_id: phone is identified by account + "phone" prefix
        val sensorId = "phone:$account"

        binding.btnSubmitBatch.isEnabled = false
        log("Submitting sensor batch ($count readings)...")

        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                runCatching {
                    // Register sensor if not done yet (idempotent on-chain)
                    try {
                        `postSensorRegister`(nodeUrl, sensorId, account, "phone", null, "")
                    } catch (_: Exception) {}

                    val sig = `signSensorCommit`(mnemonic, chainId, sensorId, account, hash, count.toULong())
                    `postSensorCommit`(nodeUrl, sensorId, account, hash, count.toULong(), "sampled", null, sig)
                }
            }.onSuccess { log("Batch submitted: $it") }
             .onFailure { log("Batch error: ${it.message}") }
            binding.btnSubmitBatch.isEnabled = true
        }
    }

    // ── Flipper frame relay ───────────────────────────────────────────────────

    // Called from FlipperFragment when a frame arrives
    fun onFlipperFrame(frame: ByteArray, flipperPubkeyHex: String) {
        if (_binding == null) return
        lifecycleScope.launch(Dispatchers.IO) {
            val parsed = runCatching { `parseBleFrame`(frame) }.getOrNull() ?: return@launch
            val verified = runCatching { `verifyBleFrameSig`(frame, flipperPubkeyHex) }.getOrElse { false }

            val frameObj = JSONObject().apply {
                put("msg_type", parsed.msgType.toInt())
                put("payload_hex", parsed.payload.joinToString("") { "%02x".format(it) })
                put("flipper_pubkey", flipperPubkeyHex)
                put("verified", verified)
                put("ts", System.currentTimeMillis())
            }

            flipperFrameBuffer.add(frameObj)

            withContext(Dispatchers.Main) {
                if (_binding == null) return@withContext
                binding.flipperBufferStatus.text = "${flipperFrameBuffer.size} frame(s) buffered · " +
                    "latest type=0x%02x · %s".format(
                        parsed.msgType.toInt(),
                        if (verified) "✓ sig" else "⚠ sig unverified"
                    )
                binding.btnRelayFlipper.isEnabled = true
                binding.btnRelayFlipper.setTextColor(0xFFf59e0b.toInt())
            }
        }
    }

    private fun relayFlipperData() {
        if (flipperFrameBuffer.isEmpty()) { log("No Flipper frames buffered."); return }
        val mnemonic = getSecurePrefs().getString("mnemonic", null) ?: run { log("Import wallet first."); return }
        val account  = getSecurePrefs().getString("account", null) ?: return
        val nodeUrl  = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"
        val chainId  = settings().getString("chain_id", "btcpc-satoshi") ?: "btcpc-satoshi"

        val frames = flipperFrameBuffer.toList()
        flipperFrameBuffer.clear()

        // relay account owns the data — Flipper device is identified by its pubkey
        val flipperPubkey = frames.firstOrNull()?.optString("flipper_pubkey", "") ?: ""
        val sensorId = "flipper:${flipperPubkey.take(16)}"

        val batch     = JSONArray(frames)
        val batchHash = sha256(batch.toString())
        val count     = frames.size

        binding.btnRelayFlipper.isEnabled = false
        log("Relaying $count Flipper frame(s)...")

        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                runCatching {
                    try {
                        `postSensorRegister`(nodeUrl, sensorId, account, "flipper", null, "")
                    } catch (_: Exception) {}
                    val sig = `signSensorCommit`(mnemonic, chainId, sensorId, account, batchHash, count.toULong())
                    `postSensorCommit`(nodeUrl, sensorId, account, batchHash, count.toULong(), "event", null, sig)
                }
            }.onSuccess {
                log("Flipper data relayed: $it")
                binding.flipperBufferStatus.text = "Relayed $count frames."
            }.onFailure { log("Relay error: ${it.message}") }
            binding.btnRelayFlipper.isEnabled = flipperFrameBuffer.isNotEmpty()
        }
    }

    // ── Capability announcement ───────────────────────────────────────────────

    private fun announceCapabilities() {
        val mnemonic = getSecurePrefs().getString("mnemonic", null) ?: run { log("Import wallet first."); return }
        val account  = getSecurePrefs().getString("account", null) ?: return
        val nodeUrl  = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"

        val pm = requireContext().packageManager
        val hasVision = pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)
        val hasAudio  = pm.hasSystemFeature(PackageManager.FEATURE_MICROPHONE)
        val epoch = System.currentTimeMillis() / 1000 / 30

        binding.btnAnnounce.isEnabled = false
        log("Announcing capabilities...")

        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                runCatching {
                    val sig = `signNodeCapability`(mnemonic, account, epoch.toULong())
                    `postNodeCapability`(nodeUrl, account, sig, epoch.toULong(), hasVision, hasAudio, false, "mobile")
                }
            }.onSuccess { log("Announced: $it") }
             .onFailure { log("Announce error: ${it.message}") }
            binding.btnAnnounce.isEnabled = true
        }
    }

    // ── Inference jobs ────────────────────────────────────────────────────────

    private fun fetchJobs() {
        val nodeUrl = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"
        binding.btnFetchJobs.isEnabled = false
        binding.jobsContainer.removeAllViews()
        log("Fetching jobs...")

        lifecycleScope.launch {
            withContext(Dispatchers.IO) { runCatching { `fetchInferenceJobs`(nodeUrl, "open") } }
                .onSuccess { renderJobs(it) }
                .onFailure { log("Error: ${it.message}") }
            binding.btnFetchJobs.isEnabled = true
        }
    }

    private fun renderJobs(json: String) {
        val array = try { JSONArray(json) } catch (_: JSONException) { log("Response: $json"); return }
        if (array.length() == 0) { log("No open jobs."); return }

        for (i in 0 until array.length()) {
            val job   = array.getJSONObject(i)
            val jobId = job.optString("job_id", "?")
            val fee   = job.optLong("fee", 0)
            val type  = job.optString("task_type", "inference")

            val card = android.widget.LinearLayout(requireContext()).apply {
                orientation = android.widget.LinearLayout.VERTICAL
                setBackgroundResource(R.drawable.card_bg)
                setPadding(32, 24, 32, 24)
            }

            card.addView(TextView(requireContext()).apply {
                text = "$type  ·  ${"%.4f".format(fee.toDouble() / 1e8)} BTCPC\n$jobId"
                setTextColor(0xFFd1d5db.toInt())
                textSize = 11f
                typeface = android.graphics.Typeface.MONOSPACE
            })

            card.addView(Button(requireContext()).apply {
                text = "Bid"
                setBackgroundColor(0xFF1f2937.toInt())
                setTextColor(0xFFf59e0b.toInt())
                setOnClickListener { bidOnJob(jobId, fee.toULong(), "worker") }
            })

            val lp = ViewGroup.MarginLayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = 12 }
            binding.jobsContainer.addView(card, lp)
        }
        log("${array.length()} job(s) loaded.")
    }

    private fun bidOnJob(jobId: String, fee: ULong, role: String) {
        val mnemonic = getSecurePrefs().getString("mnemonic", null) ?: run { log("Import wallet first."); return }
        val account  = getSecurePrefs().getString("account", null) ?: return
        val nodeUrl  = settings().getString("node_url", "http://10.0.2.2:4242") ?: "http://10.0.2.2:4242"
        val chainId  = settings().getString("chain_id", "btcpc-satoshi") ?: "btcpc-satoshi"
        val nonce    = System.currentTimeMillis().toULong()

        log("Bidding on $jobId…")
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                runCatching {
                    val sig = `signInferenceBid`(mnemonic, chainId, account, jobId, fee, role, nonce)
                    `postInferenceBid`(nodeUrl, jobId, account, fee, role, nonce, sig)
                }
            }.onSuccess { log("Bid submitted: $it") }
             .onFailure { log("Bid error: ${it.message}") }
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private fun sha256(input: String): String {
        val bytes = MessageDigest.getInstance("SHA-256").digest(input.toByteArray(Charsets.UTF_8))
        return bytes.joinToString("") { "%02x".format(it) }
    }

    private fun hasPermission(perm: String) =
        ContextCompat.checkSelfPermission(requireContext(), perm) == PackageManager.PERMISSION_GRANTED

    private fun log(msg: String) {
        if (_binding == null) return
        requireActivity().runOnUiThread { binding.nodeLog.append("$msg\n") }
    }

    private fun getSecurePrefs(): android.content.SharedPreferences {
        val keyAlias = MasterKeys.getOrCreate(MasterKeys.AES256_GCM_SPEC)
        return EncryptedSharedPreferences.create(
            "btcpc_wallet_secure", keyAlias, requireContext(),
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

    private fun settings() =
        requireActivity().getSharedPreferences("btcpc_settings", Context.MODE_PRIVATE)

    override fun onDestroyView() {
        super.onDestroyView()
        sensorManager?.unregisterListener(sensorListener)
        _binding = null
    }
}
