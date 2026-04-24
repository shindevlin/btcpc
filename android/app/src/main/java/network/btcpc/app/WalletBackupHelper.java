package network.btcpc.app;

import android.graphics.Bitmap;
import android.graphics.Color;

import com.google.zxing.BarcodeFormat;
import com.google.zxing.EncodeHintType;
import com.google.zxing.WriterException;
import com.google.zxing.common.BitMatrix;
import com.google.zxing.qrcode.QRCodeWriter;
import com.google.zxing.qrcode.decoder.ErrorCorrectionLevel;

import java.util.HashMap;
import java.util.Map;

/** Generates a QR code Bitmap from account + posting key for backup purposes. */
class WalletBackupHelper {

    static String encodePayload(String account, String postingKey) {
        return account.trim() + ":" + postingKey.trim();
    }

    /** Returns null if either field is empty or QR encoding fails. */
    static Bitmap generateQr(String account, String postingKey, int sizePx) {
        if (account == null || account.trim().isEmpty()) return null;
        if (postingKey == null || postingKey.trim().isEmpty()) return null;
        String payload = encodePayload(account, postingKey);
        try {
            Map<EncodeHintType, Object> hints = new HashMap<>();
            hints.put(EncodeHintType.ERROR_CORRECTION, ErrorCorrectionLevel.M);
            hints.put(EncodeHintType.MARGIN, 2);
            BitMatrix matrix = new QRCodeWriter().encode(payload, BarcodeFormat.QR_CODE, sizePx, sizePx, hints);
            int w = matrix.getWidth(), h = matrix.getHeight();
            int[] pixels = new int[w * h];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    pixels[y * w + x] = matrix.get(x, y) ? Color.BLACK : Color.WHITE;
            return Bitmap.createBitmap(pixels, w, h, Bitmap.Config.RGB_565);
        } catch (WriterException e) {
            return null;
        }
    }

    /** Parse a "account:postingKey" or "account:postingKey:extraStuff" string. Returns {account, key} or null. */
    static String[] parsePayload(String raw) {
        if (raw == null) return null;
        raw = raw.trim();
        int colon = raw.indexOf(':');
        if (colon < 1) return null;
        String account = raw.substring(0, colon).trim();
        String key = raw.substring(colon + 1).trim();
        if (account.isEmpty() || key.isEmpty()) return null;
        return new String[]{account, key};
    }
}
