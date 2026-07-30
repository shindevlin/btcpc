package network.hone.app.data

import android.content.Context
import org.bouncycastle.crypto.params.Ed25519PrivateKeyParameters
import org.bouncycastle.crypto.signers.Ed25519Signer
import java.io.File

/**
 * Holds the OWNER's posting key on the phone and signs sensor commits with it.
 *
 * Decision (Shin, 2026-07-30): the phone signs as the owner (josh) using josh's POSTING
 * key. Verified against the node's role checks: the posting key CANNOT transfer funds
 * (Transfer requires the "active" key, tx.rs:187) and CANNOT re-key the account (owner
 * key). So a lost phone cannot lose funds or seize the account; worst case is
 * equivocation-griefing on seals, recoverable by rotating the posting key. This is zero
 * new consensus code — commits verify against josh's already-on-chain posting key, so it
 * works against the deployed nodes with no protocol change.
 *
 * The key is PLACED by the operator, never generated here and never extracted by tooling:
 * generating a random key would be unregistered and rejected by the network. If no key is
 * placed, the node runs read-only (captures sensors, cannot submit) and says so.
 *
 * Placement: a 32-byte raw ed25519 seed (hex or raw) at `filesDir/owner_posting.seed`, or
 * pushed once via the app's import flow. app-private storage is the floor; a hardware
 * Keystore upgrade is a follow-up.
 */
class SigningKeyStore private constructor(seed: ByteArray) {

    private val priv = Ed25519PrivateKeyParameters(seed, 0)
    val publicKeyHex: String = priv.generatePublicKey().encoded.toHex()

    /** Sign a canonical signing message; 64-byte signature as lowercase hex. */
    fun sign(message: String): String {
        val s = Ed25519Signer()
        s.init(true, priv)
        val b = message.toByteArray(Charsets.UTF_8)
        s.update(b, 0, b.size)
        return s.generateSignature().toHex()
    }

    companion object {
        const val SEED_FILE = "owner_posting.seed"

        /** Returns the key store, or null if the operator has not placed a key yet. */
        fun loadOrNull(ctx: Context): SigningKeyStore? {
            val f = File(ctx.filesDir, SEED_FILE)
            if (!f.exists()) return null
            val raw = f.readBytes()
            val seed = when {
                raw.size == 32 -> raw
                // Allow a hex-encoded seed file (64 hex chars, optional whitespace).
                else -> runCatching { raw.toString(Charsets.UTF_8).trim().hexToBytes() }
                    .getOrNull()?.takeIf { it.size == 32 } ?: return null
            }
            return SigningKeyStore(seed)
        }

        /** Import a placed seed (hex or raw 32 bytes) into app-private storage. */
        fun place(ctx: Context, seedHexOrRaw: ByteArray): Boolean {
            val seed = if (seedHexOrRaw.size == 32) seedHexOrRaw
            else runCatching { seedHexOrRaw.toString(Charsets.UTF_8).trim().hexToBytes() }
                .getOrNull()?.takeIf { it.size == 32 } ?: return false
            val f = File(ctx.filesDir, SEED_FILE)
            f.writeBytes(seed)
            f.setReadable(false, false); f.setReadable(true, true)
            f.setWritable(false, false); f.setWritable(true, true)
            return true
        }
    }
}

private fun ByteArray.toHex(): String {
    val sb = StringBuilder(size * 2)
    for (b in this) { val v = b.toInt() and 0xff; sb.append(HEXC[v ushr 4]); sb.append(HEXC[v and 0x0f]) }
    return sb.toString()
}
private fun String.hexToBytes(): ByteArray {
    val clean = filter { !it.isWhitespace() }
    require(clean.length % 2 == 0) { "hex must be even length" }
    return ByteArray(clean.length / 2) { ((clean[it * 2].digitToInt(16) shl 4) or clean[it * 2 + 1].digitToInt(16)).toByte() }
}
private val HEXC = "0123456789abcdef".toCharArray()
