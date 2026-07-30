package network.hone.app.data

import android.util.Log
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/** Config for the sensor node's on-chain identity and target peer. */
data class NodeConfig(
    val chainId: String = "hone",
    val owner: String = "josh",
    val peerBaseUrl: String = "http://192.168.68.72:4242", // grouchly; any peered node works
    val sensorIdPrefix: String = "moto-gpower",            // per-device prefix → unique sensor_ids
)

sealed interface SubmitResult {
    data class Accepted(val hash: String?) : SubmitResult
    data class Rejected(val error: String) : SubmitResult
    data class Skipped(val reason: String) : SubmitResult
}

/**
 * Turns captured readings into signed, on-chain sensor commits.
 *
 * Per channel, once: POST /api/sensor/register (owner-signed). Then each tick: take the
 * batch of readings observed since the last commit, SHA-256 them into batch_hash, build the
 * EXACT canonical message (CanonicalMessage — insertion-ordered), sign it as the owner with
 * the placed posting key, and POST /api/sensor/commit. reading_count = samples this batch.
 *
 * Every commit is signed. With no key placed, submission is skipped (read-only) — never
 * unsigned. Zero new consensus code: the node verifies the signature against josh's
 * on-chain posting key.
 */
class SensorSubmitter(
    private val cfg: NodeConfig,
    private val keys: SigningKeyStore?,
) {
    private val registered = HashSet<String>()

    fun sensorId(channel: SensorChannel) = "${cfg.sensorIdPrefix}-${channel.id}"

    /** Register a channel's sensor once (idempotent on-chain and locally). */
    fun ensureRegistered(channel: SensorChannel): SubmitResult {
        val keys = keys ?: return SubmitResult.Skipped("no signing key placed")
        val sid = sensorId(channel)
        if (sid in registered) return SubmitResult.Accepted(null)
        val msg = CanonicalMessage.sensorRegister(
            cfg.chainId, sid, cfg.owner, "continuous", null, cfg.owner)
        val body = JSONObject().apply {
            put("sensor_id", sid); put("owner", cfg.owner)
            put("sensor_type", "continuous")
            put("signature", keys.sign(msg))
        }
        return post("/api/sensor/register", body).also {
            if (it is SubmitResult.Accepted) registered += sid
        }
    }

    /** Commit a batch of readings for a channel. `samples` = reading_count for this batch. */
    fun commit(channel: SensorChannel, samples: Long): SubmitResult {
        val keys = keys ?: return SubmitResult.Skipped("no signing key placed")
        if (samples <= 0) return SubmitResult.Skipped("no new readings")
        val sid = sensorId(channel)
        // batch_hash binds the representative value + count + type; full samples are
        // hashed off-chain (here, their summary) — only the hash and count go on-chain.
        val batch = "$sid|${channel.value}|$samples|${channel.honeType}"
        val batchHash = sha256Hex(batch)
        val msg = CanonicalMessage.sensorDataCommit(
            cfg.chainId, sid, cfg.owner, batchHash, samples, "continuous", cfg.owner)
        val body = JSONObject().apply {
            put("sensor_id", sid); put("owner", cfg.owner)
            put("batch_hash", batchHash); put("reading_count", samples)
            put("sensor_type", "continuous"); put("value", channel.value)
            put("signature", keys.sign(msg))
        }
        return post("/api/sensor/commit", body)
    }

    private fun post(path: String, body: JSONObject): SubmitResult {
        val url = URL(cfg.peerBaseUrl.trimEnd('/') + path)
        return try {
            (url.openConnection() as HttpURLConnection).run {
                requestMethod = "POST"
                connectTimeout = 8000; readTimeout = 8000
                doOutput = true
                setRequestProperty("Content-Type", "application/json")
                outputStream.use { it.write(body.toString().toByteArray()) }
                val code = responseCode
                val text = (if (code in 200..299) inputStream else errorStream)
                    ?.bufferedReader()?.readText().orEmpty()
                val json = runCatching { JSONObject(text) }.getOrNull()
                val accepted = json?.optBoolean("accepted", code in 200..299) ?: (code in 200..299)
                if (accepted) SubmitResult.Accepted(json?.optString("hash", null))
                else SubmitResult.Rejected(json?.optString("error", "HTTP $code") ?: "HTTP $code")
            }
        } catch (e: Exception) {
            Log.w("SensorSubmitter", "$path failed: ${e.message}")
            SubmitResult.Rejected(e.message ?: "network error")
        }
    }

    private fun sha256Hex(s: String): String =
        MessageDigest.getInstance("SHA-256").digest(s.toByteArray()).joinToString("") {
            "%02x".format(it)
        }
}
