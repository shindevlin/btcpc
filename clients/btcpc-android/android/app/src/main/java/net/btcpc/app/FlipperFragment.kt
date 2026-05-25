package net.btcpc.app

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
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

        // BtcpcFrameHeader size: magic(4) + type(1) + payload_len(2) + sig(64) = 71
        const val FRAME_HEADER_SIZE = 71
    }

    private var _binding: FragmentFlipperBinding? = null
    private val binding get() = _binding!!

    private var gatt: BluetoothGatt? = null
    private var scanner: BluetoothLeScanner? = null
    private var scanning = false
    private val foundDevices = mutableMapOf<String, BluetoothDevice>()

    // Whether the DATA_CHANNEL CCCD subscription is complete
    private var dataChannelReady = false

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
            if (foundDevices.containsKey(dev.address)) return
            foundDevices[dev.address] = dev
            val name = try { dev.name ?: "" } catch (_: SecurityException) { "" }
            addDeviceRow(dev, name, result.rssi)
        }
        override fun onScanFailed(err: Int) { log("Scan failed (code $err). Try toggling Bluetooth.") }
    }

    private fun addDeviceRow(dev: BluetoothDevice, name: String, rssi: Int) {
        requireActivity().runOnUiThread {
            val isBtcpc = name.contains("BTCPC", ignoreCase = true) ||
                          name.contains("Flipper", ignoreCase = true)
            val label = if (name.isNotEmpty()) name else dev.address
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
        log("Connecting to $name...")
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
                    log("Disconnected.")
                    dataChannelReady = false
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

        // DATA_CHANNEL notifications come here alongside SIGN_RESPONSE notifications
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            when (ch.uuid) {
                SIGN_RESP_UUID    -> displaySignature(value)
                DATA_CHANNEL_UUID -> handleDataFrame(value)
            }
        }

        @Deprecated("Deprecated in API 33")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                when (ch.uuid) {
                    SIGN_RESP_UUID    -> displaySignature(ch.value ?: byteArrayOf())
                    DATA_CHANNEL_UUID -> handleDataFrame(ch.value ?: byteArrayOf())
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            val charUuid = d.characteristic?.uuid
            when (charUuid) {
                SIGN_RESP_UUID -> {
                    log("Notifications enabled (signing).")
                    // Chain: subscribe DATA_CHANNEL next
                    enableDataChannelNotifications(g)
                }
                DATA_CHANNEL_UUID -> {
                    log("Notifications enabled (data channel).")
                    dataChannelReady = true
                    // Send clock sync as first frame so Flipper knows phone time
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
            MSG_HEARTBEAT  -> handleHeartbeat(payload)
            MSG_SUBGHZ_OBS -> handleSubGhzObs(payload)
            MSG_RFID_SCAN  -> handleRfidScan(payload)
            MSG_NFC_SCAN   -> handleNfcScan(payload)
            MSG_IBUTTON    -> handleIButton(payload)
            MSG_IR_CAPTURE -> handleIrCapture(payload)
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

    // ── Disconnect ────────────────────────────────────────────────────────────

    @SuppressLint("MissingPermission")
    private fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        dataChannelReady = false
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
