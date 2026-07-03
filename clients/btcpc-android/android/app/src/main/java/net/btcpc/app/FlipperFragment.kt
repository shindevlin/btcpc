package net.btcpc.app

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import net.btcpc.app.databinding.FragmentFlipperBinding
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID

class FlipperFragment : Fragment() {

    companion object {
        // ── GATT UUIDs (must match btcpc_ble_svc.h) ──────────────────────────
        val SVC_UUID          = UUID.fromString("7559e4f6-e38d-4013-a0a3-506a58adb3cb")
        val SIGN_REQ_UUID     = UUID.fromString("74ffefa5-0d47-4b27-b4af-a3df9f109556")
        val SIGN_RESP_UUID    = UUID.fromString("f40ee8ee-b7a6-4034-8d22-a44357cc4a45")
        val PUBKEY_UUID       = UUID.fromString("f0abf355-2cbf-41b1-8493-2f1aa7ec836f")
        val DATA_CHANNEL_UUID = UUID.fromString("a4c8e1b7-5f2d-4380-9e16-d70c1f4a2e85")
        val OTA_WRITE_UUID    = UUID.fromString("3c7f1b2a-4d5e-6f80-9a0b-c1d2e3f40516")
        val OTA_STATUS_UUID   = UUID.fromString("5e8d2c3b-6f7a-8b90-0c1d-2e3f4a5b6071")
        val CCCD_UUID         = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        // ── BtcpcMsgType values (must match btcpc_protocol.h) ─────────────────
        const val MSG_SUBGHZ_OBS  = 0x01.toByte()
        const val MSG_RFID_SCAN   = 0x02.toByte()
        const val MSG_NFC_SCAN    = 0x03.toByte()
        const val MSG_IBUTTON     = 0x04.toByte()
        const val MSG_HEARTBEAT   = 0x05.toByte()
        const val MSG_IR_CAPTURE  = 0x06.toByte()
        const val MSG_CLOCK_SYNC  = 0x11.toByte()
        const val MSG_GPS         = 0x12.toByte()
        const val MSG_SENSOR_REQ  = 0x14.toByte()

        const val MSG_SUBGHZ_CENSUS  = 0x08.toByte()
        const val MSG_BLE_ENV        = 0x09.toByte()
        const val MSG_READER_DETECT  = 0x0A.toByte()

        // BtcpcFrameHeader size: magic(4) + type(1) + payload_len(2) + sig(64) = 71
        const val FRAME_HEADER_SIZE = 71

        // SubGHz census: 13 bands × 8 bytes + 2 bytes listen_window_ms = 106 bytes
        const val CENSUS_BAND_COUNT      = 13
        const val CENSUS_BAND_BYTES      = 8
        const val CENSUS_PAYLOAD_BYTES   = CENSUS_BAND_COUNT * CENSUS_BAND_BYTES + 2

        // Band labels indexed 0–12, matching the Flipper btcpc_protocol.h band order
        val CENSUS_BAND_LABELS = arrayOf(
            "315.0",   // 0: US remotes/garage
            "418.1",   // 1: UK legacy alarms/keyfobs
            "433.4",   // 2: Somfy blinds
            "433.9",   // 3: Primary ISM (garage, TPMS, weather, alarms)
            "446.1",   // 4: PMR446 walkie-talkies
            "780.0",   // 5: LTE 700/800 MHz RSSI
            "868.1",   // 6: LoRaWAN EU ch1
            "868.3",   // 7: LoRaWAN EU ch2 / Z-Wave / EnOcean
            "868.4",   // 8: Z-Wave EU
            "868.5",   // 9: LoRaWAN EU ch3
            "868.95",  // 10: wM-Bus smart meters
            "869.85",  // 11: EU alarms / panic buttons
            "916.5"    // 12: US ERT smart meters
        )

        // Band annotation: null = no annotation for long events
        val CENSUS_BAND_ANNOTATIONS = arrayOf(
            null, null, null, null,
            " ← Radio",  // 4: PMR446
            null,
            " ← LoRa",   // 6: LoRaWAN EU ch1
            " ← LoRa",   // 7: LoRaWAN EU ch2
            null,
            " ← LoRa",   // 9: LoRaWAN EU ch3
            " ← Meter",  // 10: wM-Bus
            null, null
        )

        // Max file data per OTA chunk: 1 cmd byte + 240 payload = 241 ≤ MTU 244-3
        const val OTA_CHUNK_SIZE = 240
    }

    private var _binding: FragmentFlipperBinding? = null
    private val binding get() = _binding!!

    private var gatt: BluetoothGatt? = null
    private var scanner: BluetoothLeScanner? = null
    private var scanning = false
    private val foundDevices = mutableMapOf<String, BluetoothDevice>()

    // Whether the DATA_CHANNEL CCCD subscription is complete
    private var dataChannelReady = false
    // Whether OTA_STATUS CCCD subscription is complete
    private var otaStatusReady = false

    // OTA state machine
    private enum class OtaState { IDLE, OPENING, SENDING, COMMITTING }
    private var otaState    = OtaState.IDLE
    private var otaBytes:  ByteArray? = null
    private var otaOffset   = 0
    private var otaChecksum = 0  // u32 wrapping sum — matches Flipper uint32_t

    private val otaFilePicker = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        uri ?: return@registerForActivityResult
        try {
            val bytes = requireContext().contentResolver.openInputStream(uri)?.readBytes()
            if (bytes == null) { log("OTA: could not read file"); return@registerForActivityResult }
            otaBytes = bytes
            updateOtaStatus("Ready: ${bytes.size} bytes — tap Update Flipper")
            log("OTA: loaded ${bytes.size} bytes")
        } catch (e: Exception) {
            log("OTA: file read error: ${e.message}")
        }
    }

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        if (grants.values.all { it }) startScan() else log("BLE permissions denied.")
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, saved: Bundle?): View {
        _binding = FragmentFlipperBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        binding.btnScan.setOnClickListener { onScanClicked() }
        binding.btnSign.setOnClickListener { onSignClicked() }
        binding.btnDisconnect.setOnClickListener { disconnect() }
        binding.btnRequestSubghz.setOnClickListener  { onRequestSensor(MSG_SUBGHZ_OBS,  "Sub-GHz") }
        binding.btnRequestHeartbeat.setOnClickListener { onRequestSensor(MSG_HEARTBEAT, "Heartbeat") }
        binding.btnRequestRfid.setOnClickListener     { onRequestSensor(MSG_RFID_SCAN,  "RFID") }
        binding.btnRequestNfc.setOnClickListener      { onRequestSensor(MSG_NFC_SCAN,   "NFC") }
        binding.btnRequestIbutton.setOnClickListener  { onRequestSensor(MSG_IBUTTON,    "iButton") }
        binding.btnRequestIr.setOnClickListener       { onRequestSensor(MSG_IR_CAPTURE, "IR") }
        binding.btnSelectFap.setOnClickListener       { otaFilePicker.launch(arrayOf("*/*")) }
        binding.btnUpdateFlipper.setOnClickListener   { onUpdateFlipperClicked() }
        showConnected(false)
    }

    // ── Scanning ──────────────────────────────────────────────────────────────

    private fun onScanClicked() {
        if (scanning) { stopScan(); return }
        val needed = arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(requireContext(), it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startScan() else permLauncher.launch(missing.toTypedArray())
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        val btMgr = requireContext().getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = btMgr.adapter
        if (adapter == null || !adapter.isEnabled) { log("Bluetooth is off."); return }

        scanner = adapter.bluetoothLeScanner
        foundDevices.clear()
        binding.deviceList.removeAllViews()

        // Show bonded Flipper/BTCPC devices immediately — bonded BLE devices don't
        // reliably surface in generic LE scans due to Resolvable Private Addresses.
        try {
            adapter.bondedDevices
                .filter { dev -> isBtcpcDevice(dev, dev.name ?: "") }
                .forEach { dev ->
                    val name = dev.name ?: dev.address
                    foundDevices[dev.address] = dev
                    addDeviceRow(dev, name, 0)
                    log("Found paired: $name (${dev.address})")
                }
        } catch (_: SecurityException) {}

        // No UUID filter — Flipper puts 128-bit UUID in scan response, not primary advert.
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner?.startScan(null, settings, scanCallback)

        scanning = true
        binding.btnScan.text = "Stop Scan"
        log("Scanning for all BLE devices (looking for BTCPC-Flipper)...")
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        scanner?.stopScan(scanCallback)
        scanning = false
        binding.btnScan.text = "Scan for Flipper"
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val dev = result.device ?: return
            val name = try { dev.name ?: "" } catch (_: SecurityException) { "" }
            Log.d("FlipperFrag", "scan: addr=${dev.address} name='$name' rssi=${result.rssi}")
            if (foundDevices.containsKey(dev.address)) return
            foundDevices[dev.address] = dev
            addDeviceRow(dev, name, result.rssi)
        }
        override fun onScanFailed(err: Int) {
            Log.e("FlipperFrag", "scan failed: $err")
            log("Scan failed (code $err). Try toggling Bluetooth.")
        }
    }

    private fun isBtcpcDevice(dev: BluetoothDevice, name: String): Boolean {
        if (name.contains("BTCPC", ignoreCase = true)) return true
        if (name.contains("Flipper", ignoreCase = true)) return true
        // Flipper BTCPC profile XOR's the last 2 bytes of the factory MAC.
        // Flipper OUI prefix 80:E1:27:71 remains unchanged — use it as a fingerprint.
        val addr = dev.address.uppercase()
        return addr.startsWith("80:E1:27:71")
    }

    private fun addDeviceRow(dev: BluetoothDevice, name: String, rssi: Int) {
        requireActivity().runOnUiThread {
            val isBtcpc = isBtcpcDevice(dev, name)
            val label = when {
                name.isNotEmpty() -> name
                isBtcpc           -> "BTCPC-Flipper"
                else              -> dev.address
            }
            val btn = Button(requireContext()).apply {
                text = if (isBtcpc) "★ $label  ·  ${dev.address}  ($rssi dBm)"
                       else "$label  ·  ${dev.address}  ($rssi dBm)"
                setBackgroundColor(if (isBtcpc) 0xFF1a2a0a.toInt() else 0xFF1f2937.toInt())
                setTextColor(if (isBtcpc) 0xFFf59e0b.toInt() else 0xFF9ca3af.toInt())
                setOnClickListener { connectTo(dev, label) }
            }
            if (isBtcpc) binding.deviceList.addView(btn, 0)
            else binding.deviceList.addView(btn)
        }
    }

    // ── Connection ────────────────────────────────────────────────────────────

    @SuppressLint("MissingPermission")
    private fun connectTo(dev: BluetoothDevice, name: String) {
        stopScan()
        // Close any previous GATT handle before creating a new one.
        gatt?.close()
        gatt = null
        log("Connecting to $name...")
        // Always use autoConnect=false — the Flipper is actively advertising when the BLE scene
        // is open, so background connection mode just causes an indefinite "Connecting" hang.
        // If we get status 0x93 (stale bond), the error handler below tells the user to forget
        // the device in BT settings.
        gatt = try {
            dev.connectGatt(requireContext(), false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) {
            log("Connect permission denied."); null
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    log("Connected. Negotiating MTU...")
                    // Request MTU 244 first; discoverServices() is called in onMtuChanged.
                    g.requestMtu(244)
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    if (status != BluetoothGatt.GATT_SUCCESS) {
                        val hint = when (status) {
                            0x93 -> " (bonding mismatch — forget device in BT settings and retry)"
                            133  -> " (device not found — make sure Flipper BLE scene is open)"
                            else -> " (status=0x%02x)".format(status)
                        }
                        log("Connection failed.$hint")
                    } else {
                        log("Disconnected.")
                    }
                    dataChannelReady = false
                    otaStatusReady   = false
                    otaState         = OtaState.IDLE
                    g.close()
                    requireActivity().runOnUiThread { showConnected(false) }
                    gatt = null
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            log("MTU: $mtu bytes. Discovering services...")
            g.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(SVC_UUID)
            if (svc == null) { log("BTCPC service not found on device."); return }
            val pkChar = svc.getCharacteristic(PUBKEY_UUID) ?: return
            g.readCharacteristic(pkChar)
        }

        @SuppressLint("MissingPermission")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int
        ) {
            if (ch.uuid == PUBKEY_UUID && status == BluetoothGatt.GATT_SUCCESS) {
                val hex = value.joinToString("") { "%02x".format(it) }
                requireActivity().runOnUiThread {
                    binding.pubkeyText.text = hex
                    showConnected(true)
                    log("Pubkey: ${hex.take(16)}…")
                }
                // Chain: subscribe SIGN_RESP notifications first
                enableSignRespNotifications(g)
            }
        }

        @Deprecated("Deprecated in API 33")
        @Suppress("DEPRECATION")
        @SuppressLint("MissingPermission")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                onCharacteristicRead(g, ch, ch.value ?: byteArrayOf(), status)
            }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            when (ch.uuid) {
                SIGN_RESP_UUID    -> displaySignature(value)
                DATA_CHANNEL_UUID -> handleDataFrame(value)
                OTA_STATUS_UUID   -> handleOtaStatus(value)
            }
        }

        @Deprecated("Deprecated in API 33")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                when (ch.uuid) {
                    SIGN_RESP_UUID    -> displaySignature(ch.value ?: byteArrayOf())
                    DATA_CHANNEL_UUID -> handleDataFrame(ch.value ?: byteArrayOf())
                    OTA_STATUS_UUID   -> handleOtaStatus(ch.value ?: byteArrayOf())
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            val charUuid = d.characteristic?.uuid
            when (charUuid) {
                SIGN_RESP_UUID -> {
                    log("Notifications enabled (signing).")
                    enableDataChannelNotifications(g)
                }
                DATA_CHANNEL_UUID -> {
                    log("Notifications enabled (data channel).")
                    dataChannelReady = true
                    // Chain: subscribe OTA_STATUS before sending clock sync
                    enableOtaStatusNotifications(g)
                }
                OTA_STATUS_UUID -> {
                    log("Notifications enabled (OTA status).")
                    otaStatusReady = true
                    sendClockSync(g)
                }
            }
        }
    }

    // ── GATT notification subscription (chained: SIGN_RESP → DATA_CHANNEL) ──

    @SuppressLint("MissingPermission")
    private fun enableSignRespNotifications(g: BluetoothGatt) {
        val svc  = g.getService(SVC_UUID) ?: return
        val ch   = svc.getCharacteristic(SIGN_RESP_UUID) ?: return
        g.setCharacteristicNotification(ch, true)
        val desc = ch.getDescriptor(CCCD_UUID) ?: return
        writeDescriptorCompat(g, desc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
    }

    @SuppressLint("MissingPermission")
    private fun enableDataChannelNotifications(g: BluetoothGatt) {
        val svc  = g.getService(SVC_UUID) ?: return
        val ch   = svc.getCharacteristic(DATA_CHANNEL_UUID) ?: return
        g.setCharacteristicNotification(ch, true)
        val desc = ch.getDescriptor(CCCD_UUID) ?: return
        writeDescriptorCompat(g, desc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
    }

    @SuppressLint("MissingPermission")
    private fun enableOtaStatusNotifications(g: BluetoothGatt) {
        val svc  = g.getService(SVC_UUID) ?: return
        val ch   = svc.getCharacteristic(OTA_STATUS_UUID) ?: return
        g.setCharacteristicNotification(ch, true)
        val desc = ch.getDescriptor(CCCD_UUID) ?: return
        writeDescriptorCompat(g, desc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
    }

    @SuppressLint("MissingPermission")
    private fun writeDescriptorCompat(g: BluetoothGatt, desc: BluetoothGattDescriptor, value: ByteArray) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(desc, value)
        } else {
            @Suppress("DEPRECATION")
            desc.value = value
            @Suppress("DEPRECATION")
            g.writeDescriptor(desc)
        }
    }

    // ── Phone → Flipper frame building ────────────────────────────────────────

    /**
     * Build a BtcpcFrame for phone → Flipper direction.
     * Phone→Flipper frames use the same header format but the sig field is zeroed
     * (Flipper firmware does not verify phone-side signatures per btcpc_protocol.c).
     *
     * Header layout: magic(4) + type(1) + payload_len_le(2) + sig_zeros(64) = 71 bytes.
     */
    private fun buildPhoneFrame(msgType: Byte, payload: ByteArray): ByteArray {
        val frame = ByteArray(FRAME_HEADER_SIZE + payload.size)
        frame[0] = 'B'.code.toByte()
        frame[1] = 'T'.code.toByte()
        frame[2] = 'P'.code.toByte()
        frame[3] = 'C'.code.toByte()
        frame[4] = msgType
        // payload_len: uint16_t little-endian
        frame[5] = (payload.size and 0xFF).toByte()
        frame[6] = ((payload.size ushr 8) and 0xFF).toByte()
        // frame[7..70] = zeros (sig placeholder — phone→Flipper is unsigned)
        payload.copyInto(frame, FRAME_HEADER_SIZE)
        return frame
    }

    /** ClockSync (0x11): 8-byte uint64_t LE unix time in milliseconds. */
    private fun buildClockSyncFrame(): ByteArray {
        val payload = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
            .putLong(System.currentTimeMillis())
            .array()
        return buildPhoneFrame(MSG_CLOCK_SYNC, payload)
    }

    /** SensorReq (0x14): 1-byte sensor_type + 2-byte duration_ms LE. */
    private fun buildSensorReqFrame(sensorType: Byte, durationMs: Short = 0): ByteArray {
        val payload = ByteBuffer.allocate(3).order(ByteOrder.LITTLE_ENDIAN)
            .put(sensorType)
            .putShort(durationMs)
            .array()
        return buildPhoneFrame(MSG_SENSOR_REQ, payload)
    }

    @SuppressLint("MissingPermission")
    private fun writeDataChannel(g: BluetoothGatt, frame: ByteArray) {
        val svc = g.getService(SVC_UUID) ?: return
        val ch  = svc.getCharacteristic(DATA_CHANNEL_UUID) ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(ch, frame, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
        } else {
            @Suppress("DEPRECATION")
            ch.value = frame
            @Suppress("DEPRECATION")
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            g.writeCharacteristic(ch)
        }
    }

    @SuppressLint("MissingPermission")
    private fun sendClockSync(g: BluetoothGatt) {
        writeDataChannel(g, buildClockSyncFrame())
        log("Sent clock sync to Flipper.")
    }

    // ── Sensor request buttons ────────────────────────────────────────────────

    private fun onRequestSensor(sensorType: Byte, name: String) {
        val g = gatt ?: run { log("Not connected."); return }
        if (!dataChannelReady) { log("Data channel not ready."); return }
        val duration: Short = if (sensorType == MSG_SUBGHZ_OBS) 500 else 0
        writeDataChannel(g, buildSensorReqFrame(sensorType, duration))
        log("Requested $name from Flipper...")
    }

    // ── Flipper → Phone frame handling ────────────────────────────────────────

    private fun handleDataFrame(raw: ByteArray) {
        if (raw.size < FRAME_HEADER_SIZE) return
        // Verify magic
        if (raw[0] != 'B'.code.toByte() || raw[1] != 'T'.code.toByte() ||
            raw[2] != 'P'.code.toByte() || raw[3] != 'C'.code.toByte()) return

        val msgType = raw[4]
        val payloadLen = (raw[5].toInt() and 0xFF) or ((raw[6].toInt() and 0xFF) shl 8)
        if (raw.size < FRAME_HEADER_SIZE + payloadLen) return

        val payload = raw.copyOfRange(FRAME_HEADER_SIZE, FRAME_HEADER_SIZE + payloadLen)

        when (msgType) {
            MSG_HEARTBEAT     -> handleHeartbeat(payload)
            MSG_SUBGHZ_OBS    -> handleSubGhzObs(payload)
            MSG_RFID_SCAN     -> handleRfidScan(payload)
            MSG_NFC_SCAN      -> handleNfcScan(payload)
            MSG_IBUTTON       -> handleIButton(payload)
            MSG_IR_CAPTURE    -> handleIrCapture(payload)
            MSG_SUBGHZ_CENSUS -> handleSubGhzCensus(payload)
            MSG_BLE_ENV       -> handleBleEnv(payload)
            MSG_READER_DETECT -> handleReaderDetect(payload)
            else -> log("Flipper data: type=0x%02x len=%d".format(msgType, payloadLen))
        }

        // Route sensor frames to NodeFragment for chain submission
        val pubkeyHex = _binding?.pubkeyText?.text?.toString() ?: ""
        val nodeFragment = parentFragmentManager.fragments
            .filterIsInstance<NodeFragment>().firstOrNull()
        nodeFragment?.onFlipperFrame(raw, pubkeyHex)
    }

    private fun handleHeartbeat(payload: ByteArray) {
        // BtcpcHeartbeat: battery_pct(1) + uptime_s(4) + fw_version(8)
        if (payload.size < 5) return
        val batteryPct = payload[0].toInt() and 0xFF
        val uptimeS = ByteBuffer.wrap(payload, 1, 4).order(ByteOrder.LITTLE_ENDIAN).int
        val fwVersion = if (payload.size >= 13) {
            payload.copyOfRange(5, minOf(13, payload.size)).toString(Charsets.UTF_8).trimEnd(Char(0), ' ')
        } else "?"
        val text = "Heartbeat\nBattery: $batteryPct%  Uptime: ${uptimeS}s\nFW: $fwVersion"
        updateSensorText(text)
        log("Flipper heartbeat: ${batteryPct}% bat, ${uptimeS}s uptime")
    }

    private fun handleSubGhzObs(payload: ByteArray) {
        // BtcpcSubGhzObs: freq_hz(4) + rssi_dbm(1) + modulation(1) + bandwidth(1) = 7 bytes
        if (payload.size < 7) return
        val buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val freqHz   = buf.int.toLong() and 0xFFFFFFFFL
        val rssiDbm  = payload[4].toInt()
        val modNames = arrayOf("AM", "FM", "OOK")
        val modByte  = payload[5].toInt() and 0xFF
        val modName  = if (modByte < modNames.size) modNames[modByte] else "?"
        val freqMhz  = "%.3f".format(freqHz / 1_000_000.0)
        val text = "Sub-GHz Observation\n$freqMhz MHz  RSSI: ${rssiDbm} dBm  $modName"
        updateSensorText(text)
        log("SubGhz: $freqMhz MHz  $rssiDbm dBm  $modName")
    }

    private fun handleRfidScan(payload: ByteArray) {
        // BtcpcRfidScan: protocol(1) + obs_id(16) = 17 bytes
        if (payload.size < 17) return
        val protocol  = payload[0].toInt() and 0xFF
        val obsHex    = payload.copyOfRange(1, 9).joinToString("") { "%02x".format(it) }
        val protoName = when (protocol) {
            0    -> "EM4100"
            1    -> "HID"
            2    -> "Indala"
            3    -> "Hitag"
            0xFF -> "unknown"
            else -> "0x%02x".format(protocol)
        }
        val text = "RFID Scan\nProtocol: $protoName\nobs: $obsHex…"
        updateSensorText(text)
        log("RFID: $protoName  obs=$obsHex…")
    }

    private fun handleNfcScan(payload: ByteArray) {
        // BtcpcNfcScan: tech(1) + tag_family(1) + obs_id(16) = 18 bytes
        if (payload.size < 18) return
        val tech      = payload[0].toInt() and 0xFF
        val family    = payload[1].toInt() and 0xFF
        val obsHex    = payload.copyOfRange(2, 10).joinToString("") { "%02x".format(it) }
        val techName  = when (tech) {
            0 -> "Type A"
            1 -> "Type B"
            2 -> "Type F (FeliCa)"
            3 -> "Type V (ISO15693)"
            else -> "0x%02x".format(tech)
        }
        val familyName = when (family) {
            0 -> "unknown"
            1 -> "MIFARE"
            2 -> "NTAG/UL"
            3 -> "DESFire"
            4 -> "FeliCa"
            5 -> "ISO15693"
            else -> "0x%02x".format(family)
        }
        val text = "NFC Scan\n$techName / $familyName\nobs: $obsHex…"
        updateSensorText(text)
        log("NFC: $techName / $familyName  obs=$obsHex…")
    }

    private fun handleIButton(payload: ByteArray) {
        // BtcpcIButton: family(1) + obs_id(16) = 17 bytes
        if (payload.size < 17) return
        val family     = payload[0].toInt() and 0xFF
        val obsHex     = payload.copyOfRange(1, 9).joinToString("") { "%02x".format(it) }
        val familyName = when (family) {
            0x01 -> "DS1990A (iKey)"
            0x28 -> "DS18B20 (Temp)"
            0x29 -> "DS2431 (EEPROM)"
            0x14 -> "DS2430A"
            else -> "0x%02x".format(family)
        }
        val text = "iButton\nFamily: $familyName\nobs: $obsHex…"
        updateSensorText(text)
        log("iButton: $familyName  obs=$obsHex…")
    }

    private fun handleIrCapture(payload: ByteArray) {
        // BtcpcIrCapture: protocol(1) + obs_id(16) = 17 bytes
        if (payload.size < 17) return
        val protocol  = payload[0].toInt() and 0xFF
        val obsHex    = payload.copyOfRange(1, 9).joinToString("") { "%02x".format(it) }
        val protoName = when (protocol) {
            0    -> "NEC"
            1    -> "Samsung32"
            2    -> "RC6"
            3    -> "RC5"
            4    -> "SIRC"
            5    -> "Kaseikyo"
            0xFF -> "raw/unknown"
            else -> "0x%02x".format(protocol)
        }
        val text = "IR Capture\nProtocol: $protoName\nobs: $obsHex…"
        updateSensorText(text)
        log("IR: $protoName  obs=$obsHex…")
    }

    // ── SubGHz Census (0x08) ──────────────────────────────────────────────────

    private data class CensusBand(
        val label: String,
        val freqHz: Long,
        val eventCount: Int,
        val longCount: Int,
        val peakRssiDbm: Int,
        val noiseFloorDbm: Int
    )

    private fun handleSubGhzCensus(payload: ByteArray) {
        if (payload.size < CENSUS_PAYLOAD_BYTES) {
            log("SubGHz census: short payload (${payload.size} < $CENSUS_PAYLOAD_BYTES)")
            return
        }

        val buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val bands = ArrayList<CensusBand>(CENSUS_BAND_COUNT)

        for (i in 0 until CENSUS_BAND_COUNT) {
            val freqHz     = buf.int.toLong() and 0xFFFFFFFFL
            val eventCount = buf.get().toInt() and 0xFF
            val longCount  = buf.get().toInt() and 0xFF
            val peakRssi   = buf.get().toInt()  // int8_t
            val noiseFloor = buf.get().toInt()  // int8_t
            bands.add(CensusBand(CENSUS_BAND_LABELS[i], freqHz, eventCount, longCount, peakRssi, noiseFloor))
        }
        val listenWindowMs = buf.short.toInt() and 0xFFFF

        // Build display text — only show bands with event_count > 0
        val activeBands = bands.filter { it.eventCount > 0 }
        val displayText = if (activeBands.isEmpty()) {
            "No RF activity detected"
        } else {
            activeBands.joinToString("\n") { band ->
                val idx = bands.indexOf(band)
                val annotation = if (band.longCount > 0) (CENSUS_BAND_ANNOTATIONS[idx] ?: "") else ""
                "%s  %dev %dL %ddBm%s".format(
                    band.label, band.eventCount, band.longCount, band.peakRssiDbm, annotation
                )
            }
        }

        log("RF census: ${activeBands.size}/${CENSUS_BAND_COUNT} bands active  listen=${listenWindowMs}ms")

        // Determine highest-priority toast — evaluate in order, fire at most one
        val toastMsg = when {
            bands[6].longCount > 0 || bands[7].longCount > 0 || bands[9].longCount > 0 ->
                "LoRa node detected nearby"
            bands[10].longCount > 0 ->
                "Smart meter detected"
            bands[4].eventCount > 0 ->
                "Radio activity detected"
            bands[3].eventCount > 3 ->
                "Active RF area — garage/sensors/remotes"
            bands[5].eventCount > 0 ->
                "Cellular coverage detected"
            else -> null
        }

        requireActivity().runOnUiThread {
            _binding?.censusText?.text = displayText
            if (toastMsg != null) {
                Toast.makeText(requireContext(), toastMsg, Toast.LENGTH_SHORT).show()
            }
        }
    }

    // ── BLE Environment (0x09) ────────────────────────────────────────────────

    private fun handleBleEnv(payload: ByteArray) {
        if (payload.size < 6) return
        val adCount        = payload[0].toInt() and 0xFF
        val scanWindowS    = payload[1].toInt() and 0xFF
        val rssiMin        = payload[2].toInt()  // int8_t
        val rssiMax        = payload[3].toInt()  // int8_t
        val rssiAvg        = payload[4].toInt()  // int8_t
        val connectableCount = payload[5].toInt() and 0xFF

        // scan_window_s == 0 means stub not yet implemented on Flipper — ignore silently
        if (scanWindowS == 0) return

        if (adCount == 0) {
            updateSensorText("BLE Environment\nNo devices in range")
        } else {
            val text = buildString {
                append("BLE Environment\n")
                append("Nearby: $adCount device${if (adCount == 1) "" else "s"}")
                if (connectableCount > 0) append("  ($connectableCount connectable)")
                append("\nRSSI min/avg/max: ${rssiMin}/${rssiAvg}/${rssiMax} dBm")
            }
            updateSensorText(text)
        }
        log("BLE env: $adCount devices  RSSI $rssiMin/$rssiAvg/$rssiMax dBm  scan=${scanWindowS}s")
    }

    // ── Reader Detect (0x0A) ──────────────────────────────────────────────────

    private fun handleReaderDetect(payload: ByteArray) {
        if (payload.size < 2) return
        val readerType = payload[0].toInt() and 0xFF
        val fieldRssi  = payload[1].toInt()  // int8_t

        val toastMsg = when (readerType) {
            0x01 -> "RFID reader detected — access control infrastructure"
            0x02 -> "NFC reader detected — payment or access terminal"
            0x03 -> "iButton reader detected — access control system"
            else -> null
        }
        val typeName = when (readerType) {
            0x01 -> "RFID reader"
            0x02 -> "NFC reader"
            0x03 -> "iButton reader"
            else -> "reader type=0x%02x".format(readerType)
        }
        log("Reader detect: $typeName  field RSSI ${fieldRssi} dBm")
        if (toastMsg != null) {
            requireActivity().runOnUiThread {
                if (_binding != null) {
                    Toast.makeText(requireContext(), toastMsg, Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    private fun updateSensorText(text: String) {
        requireActivity().runOnUiThread {
            _binding?.flipperSensorText?.text = text
        }
    }

    // ── Signing (existing logic, unchanged) ───────────────────────────────────

    private fun displaySignature(value: ByteArray) {
        val hex = value.joinToString("") { "%02x".format(it) }
        requireActivity().runOnUiThread {
            binding.sigText.text = hex
            log("Signature (${value.size}B): ${hex.take(32)}…")
        }
    }

    @SuppressLint("MissingPermission")
    private fun onSignClicked() {
        val g = gatt ?: run { log("Not connected."); return }
        val msg = binding.signInput.text.toString().trim()
        if (msg.isEmpty()) return

        val svc = g.getService(SVC_UUID) ?: run { log("Service not found."); return }
        val ch  = svc.getCharacteristic(SIGN_REQ_UUID) ?: run { log("SIGN_REQUEST char missing."); return }

        val hash = MessageDigest.getInstance("SHA-256").digest(msg.toByteArray(Charsets.UTF_8))
        log("Sending SHA-256 hash of \"$msg\"…")

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(ch, hash, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
        } else {
            @Suppress("DEPRECATION")
            ch.value = hash
            @Suppress("DEPRECATION")
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            g.writeCharacteristic(ch)
        }
    }

    // ── OTA firmware update ───────────────────────────────────────────────────

    private fun onUpdateFlipperClicked() {
        val g = gatt ?: run { log("Not connected."); return }
        val bytes = otaBytes ?: run { log("No .fap file selected."); return }
        if (!otaStatusReady) { log("OTA channel not ready."); return }
        if (otaState != OtaState.IDLE) { log("OTA already in progress."); return }

        otaOffset   = 0
        otaChecksum = 0
        otaState    = OtaState.OPENING

        val cmd = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN)
            .put('O'.code.toByte())
            .putInt(bytes.size)
            .array()
        writeOtaChar(g, cmd)
        updateOtaStatus("OTA: opening (${bytes.size} bytes)…")
        log("OTA: sent open (${bytes.size} bytes)")
    }

    private fun handleOtaStatus(value: ByteArray) {
        if (value.isEmpty()) return
        val g = gatt ?: return
        when {
            value[0] == 'K'.code.toByte() -> when (otaState) {
                OtaState.OPENING -> { otaState = OtaState.SENDING; sendNextChunk(g) }
                OtaState.SENDING -> sendNextChunk(g)
                else -> {}
            }
            value[0] == 'Z'.code.toByte() -> {
                otaState = OtaState.IDLE
                updateOtaStatus("OTA complete!")
                log("OTA: firmware update complete")
            }
            value[0] == 'E'.code.toByte() -> {
                val code = if (value.size >= 2) value[1].toInt() and 0xFF else 0
                val reason = when (code) {
                    0x01 -> "open failed"
                    0x02 -> "write error"
                    0x03 -> "size mismatch"
                    0x04 -> "checksum mismatch"
                    0x05 -> "rename failed"
                    else -> "error 0x%02x".format(code)
                }
                otaState = OtaState.IDLE
                updateOtaStatus("OTA failed: $reason")
                log("OTA: failed — $reason")
            }
        }
    }

    private fun sendNextChunk(g: BluetoothGatt) {
        val bytes = otaBytes ?: return
        if (otaOffset >= bytes.size) {
            // All chunks sent — commit
            otaState = OtaState.COMMITTING
            val cmd = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN)
                .put('C'.code.toByte())
                .putInt(otaChecksum)
                .array()
            writeOtaChar(g, cmd)
            updateOtaStatus("OTA: committing…")
            log("OTA: sent commit (checksum=0x%08x)".format(otaChecksum))
            return
        }

        val chunkLen = minOf(OTA_CHUNK_SIZE, bytes.size - otaOffset)
        val payload  = bytes.copyOfRange(otaOffset, otaOffset + chunkLen)

        for (b in payload) otaChecksum += b.toInt() and 0xFF

        val cmd = ByteArray(1 + chunkLen)
        cmd[0] = 'D'.code.toByte()
        payload.copyInto(cmd, 1)
        writeOtaChar(g, cmd)

        otaOffset += chunkLen
        val pct = (otaOffset.toLong() * 100 / bytes.size).toInt()
        updateOtaStatus("OTA: $otaOffset / ${bytes.size} bytes ($pct%)")
    }

    @SuppressLint("MissingPermission")
    private fun writeOtaChar(g: BluetoothGatt, data: ByteArray) {
        val svc = g.getService(SVC_UUID) ?: return
        val ch  = svc.getCharacteristic(OTA_WRITE_UUID) ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(ch, data, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
        } else {
            @Suppress("DEPRECATION")
            ch.value = data
            @Suppress("DEPRECATION")
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            g.writeCharacteristic(ch)
        }
    }

    private fun updateOtaStatus(text: String) {
        requireActivity().runOnUiThread {
            _binding?.otaStatusText?.text = text
        }
    }

    // ── Disconnect ────────────────────────────────────────────────────────────

    @SuppressLint("MissingPermission")
    private fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        dataChannelReady = false
        otaStatusReady   = false
        otaState         = OtaState.IDLE
        showConnected(false)
        log("Disconnected.")
    }

    private fun showConnected(connected: Boolean) {
        if (_binding == null) return
        binding.scanView.visibility      = if (connected) View.GONE else View.VISIBLE
        binding.connectedView.visibility = if (connected) View.VISIBLE else View.GONE
    }

    private fun log(msg: String) {
        if (_binding == null) return
        requireActivity().runOnUiThread {
            binding.logText.append("$msg\n")
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        try { gatt?.close() } catch (_: SecurityException) {}
        gatt = null
        _binding = null
    }
}
