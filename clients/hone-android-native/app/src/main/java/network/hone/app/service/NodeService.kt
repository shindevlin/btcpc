package network.hone.app.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import network.hone.app.data.NodeConfig
import network.hone.app.data.SensorCapture
import network.hone.app.data.SensorSubmitter
import network.hone.app.data.SubmitResult
import network.hone.app.data.SigningKeyStore

/**
 * Foreground service that KEEPS THE HONE SENSOR NODE ALIVE in the background — the piece a
 * webview cannot do (Android kills a webview when it loses focus).
 *
 * Running behaviour:
 *   - captures every sensor the device has (SensorCapture),
 *   - once per COMMIT_INTERVAL, for each available channel with new readings, builds a
 *     signed on-chain commit (owner-signed with the placed posting key) and submits it,
 *   - holds a partial wakelock across each submit tick so the CPU doesn't sleep mid-batch,
 *   - reports live status in the ongoing notification.
 *
 * specialUse FGS (not dataSync) so Android 15+ does not force-stop it after ~6h/day.
 * START_STICKY + BootReceiver so it self-heals after a kill or reboot.
 */
class NodeService : Service() {

    private val scope = CoroutineScope(Dispatchers.IO)
    private var loop: Job? = null
    private lateinit var capture: SensorCapture
    private var submitter: SensorSubmitter? = null
    private var hasKey: Boolean = false
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onCreate() {
        super.onCreate()
        capture = SensorCapture(applicationContext)
        val keys = SigningKeyStore.loadOrNull(applicationContext)
        hasKey = keys != null
        submitter = SensorSubmitter(NodeConfig(), keys)
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "hone:sensor-node")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat(buildNotification("Starting sensor node…"))
        capture.start()
        if (loop?.isActive != true) loop = scope.launch { runLoop() }
        return START_STICKY
    }

    private suspend fun runLoop() {
        val sub = submitter ?: return
        while (scope.isActive) {
            val channels = capture.channels.value.filter { it.available }
            var accepted = 0; var attempted = 0; var lastErr: String? = null
            wakeLock?.acquire(30_000L)
            try {
                for (ch in channels) {
                    val n = capture.drainSampleCount(ch.id)
                    if (n <= 0L) continue
                    attempted++
                    sub.ensureRegistered(ch)
                    when (val r = sub.commit(ch, n)) {
                        is SubmitResult.Accepted -> accepted++
                        is SubmitResult.Rejected -> lastErr = r.error
                        is SubmitResult.Skipped -> lastErr = r.reason
                    }
                }
            } finally {
                if (wakeLock?.isHeld == true) wakeLock?.release()
            }
            val avail = capture.availableCount()
            val status = when {
                !hasKey -> "$avail sensors · read-only (no key placed)"
                attempted == 0 -> "$avail sensors · warming up"
                else -> "$avail sensors · $accepted/$attempted committed" +
                        (lastErr?.let { " · $it" } ?: "")
            }
            updateNotification(status)
            delay(COMMIT_INTERVAL_MS)
        }
    }

    override fun onDestroy() {
        capture.stop()
        loop?.cancel()
        scope.cancel()
        if (wakeLock?.isHeld == true) wakeLock?.release()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun startForegroundCompat(n: Notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun buildNotification(status: String): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("HONE sensor node")
            .setContentText(status)
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()

    private fun updateNotification(status: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification(status))
    }

    companion object {
        private const val CHANNEL_ID = "hone_node"
        private const val NOTIF_ID = 1001
        private const val COMMIT_INTERVAL_MS = 30_000L // one commit cycle per ~epoch

        fun ensureChannel(ctx: Context) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                val channel = NotificationChannel(
                    CHANNEL_ID, "HONE node", NotificationManager.IMPORTANCE_LOW,
                ).apply { description = "Keeps your sensor node submitting in the background." }
                ctx.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
            }
        }

        fun start(ctx: Context) {
            ensureChannel(ctx)
            val intent = Intent(ctx, NodeService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(intent)
            else ctx.startService(intent)
        }

        fun stop(ctx: Context) { ctx.stopService(Intent(ctx, NodeService::class.java)) }
    }
}
