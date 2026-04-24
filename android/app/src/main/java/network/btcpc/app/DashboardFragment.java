package network.btcpc.app;

import android.app.AlertDialog;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.ResponseBody;

public class DashboardFragment extends Fragment {

    private static final long REFRESH_MS = 5000;
    private static final long EPOCH_DURATION_MS = 30_000L;
    private static final long EPOCH_ZERO_MS = 1776236400000L;

    private AppPrefs prefs;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean polling = false;

    private final OkHttpClient http = new OkHttpClient();

    // Phone status cards
    private View cardClock, cardMiner, cardSensors, cardStorage;
    private TextView tvClockDot, tvClockTitle, tvClockStatus;
    private TextView tvMinerDot, tvMinerTitle, tvMinerStatus;
    private TextView tvSensorsDot, tvSensorsTitle, tvSensorsStatus;
    private TextView tvStorageDot, tvStorageTitle, tvStorageStatus;

    // Chain stats
    private TextView tvEpoch, tvPeers, tvMiners, tvClocks, tvNodes, tvStatus;
    private TextView tvSensorsCount, tvGateways, tvVerifiers;
    private ProgressBar epochProgress;
    private TextView tvEpochProgress;

    // Sync indicator
    private TextView tvSyncStatus;

    // Clickable tiles
    private View tileMiners, tileClocks, tileSensors, tileGateways, tileVerifiers;

    // Last known chain epoch (from network response) for progress bar calculation
    private long lastNetworkEpoch = 0;

    @Override
    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_dashboard, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        prefs = new AppPrefs(requireContext());

        // Phone cards
        cardClock   = view.findViewById(R.id.card_clock);
        cardMiner   = view.findViewById(R.id.card_miner);
        cardSensors = view.findViewById(R.id.card_sensors);
        cardStorage = view.findViewById(R.id.card_storage);

        tvClockDot    = view.findViewById(R.id.clock_dot);
        tvClockTitle  = view.findViewById(R.id.clock_title);
        tvClockStatus = view.findViewById(R.id.clock_status);

        tvMinerDot    = view.findViewById(R.id.miner_dot);
        tvMinerTitle  = view.findViewById(R.id.miner_title);
        tvMinerStatus = view.findViewById(R.id.miner_status);

        tvSensorsDot    = view.findViewById(R.id.sensors_dot);
        tvSensorsTitle  = view.findViewById(R.id.sensors_title);
        tvSensorsStatus = view.findViewById(R.id.sensors_status);

        tvStorageDot    = view.findViewById(R.id.storage_dot);
        tvStorageTitle  = view.findViewById(R.id.storage_title);
        tvStorageStatus = view.findViewById(R.id.storage_status);

        // Chain stats
        tvEpoch         = view.findViewById(R.id.stat_epoch);
        tvPeers         = view.findViewById(R.id.stat_peers);
        tvMiners        = view.findViewById(R.id.stat_miners);
        tvClocks        = view.findViewById(R.id.stat_clocks);
        tvNodes         = view.findViewById(R.id.stat_nodes);
        tvStatus        = view.findViewById(R.id.stat_status);
        tvSensorsCount  = view.findViewById(R.id.stat_sensors);
        tvGateways      = view.findViewById(R.id.stat_gateways);
        tvVerifiers     = view.findViewById(R.id.stat_verifiers);
        epochProgress   = view.findViewById(R.id.epoch_progress);
        tvEpochProgress = view.findViewById(R.id.epoch_progress_label);

        tvSyncStatus = view.findViewById(R.id.sync_status);

        // Clickable tiles
        tileMiners   = view.findViewById(R.id.tile_miners);
        tileClocks   = view.findViewById(R.id.tile_clocks);
        tileSensors  = view.findViewById(R.id.tile_sensors);
        tileGateways = view.findViewById(R.id.tile_gateways);
        tileVerifiers = view.findViewById(R.id.tile_verifiers);

        if (tileMiners   != null) tileMiners.setOnClickListener(v -> fetchAndShowNodeDetail("miners",    "Miners"));
        if (tileClocks   != null) tileClocks.setOnClickListener(v -> fetchAndShowNodeDetail("clocks",    "Clocks"));
        if (tileSensors  != null) tileSensors.setOnClickListener(v -> fetchAndShowNodeDetail("sensors",  "Sensors"));
        if (tileGateways != null) tileGateways.setOnClickListener(v -> fetchAndShowNodeDetail("gateways","Gateways"));
        if (tileVerifiers != null) tileVerifiers.setOnClickListener(v -> fetchAndShowNodeDetail("verifiers","Verifiers"));
    }

    @Override
    public void onResume() {
        super.onResume();
        startPolling();
    }

    @Override
    public void onPause() {
        super.onPause();
        stopPolling();
    }

    private void startPolling() {
        if (polling) return;
        polling = true;
        handler.post(pollRunnable);
    }

    private void stopPolling() {
        polling = false;
        handler.removeCallbacks(pollRunnable);
    }

    private final Runnable pollRunnable = new Runnable() {
        @Override
        public void run() {
            if (!isAdded() || !polling) return;
            refreshPhoneStatus();
            fetchChainStats();
            handler.postDelayed(this, REFRESH_MS);
        }
    };

    // ---- phone status (from SharedPrefs written by services) ----

    private void refreshPhoneStatus() {
        if (!isAdded() || prefs == null) return;

        boolean clockOn   = prefs.isClockEnabled();
        boolean minerOn   = prefs.isMinerEnabled();
        boolean sensorsOn = prefs.isSensorsEnabled();
        boolean storageOn = prefs.isStorageEnabled();

        String clockState   = prefs.getClockState();
        String minerState   = prefs.getMinerState();
        String sensorState  = prefs.getSensorState();
        String storageState = prefs.getStorageState();

        applyCard(tvClockDot,   tvClockTitle,   tvClockStatus,
                clockOn, "Clock", clockState.isEmpty() ? "Stopped" : clockState);
        applyCard(tvMinerDot,   tvMinerTitle,   tvMinerStatus,
                minerOn, "Miner", minerState.isEmpty() ? "Stopped" : minerState);
        applyCard(tvSensorsDot, tvSensorsTitle, tvSensorsStatus,
                sensorsOn, "Sensors", sensorState.isEmpty() ? "Stopped" : sensorState);
        applyCard(tvStorageDot, tvStorageTitle, tvStorageStatus,
                storageOn, "Storage", storageState.isEmpty() ? "Stopped" : storageState);

        // Epoch progress bar: use chain epoch from last network response.
        // If we haven't received a network response yet, compute from wall clock as fallback.
        long now = System.currentTimeMillis();
        long baseEpoch = lastNetworkEpoch > 0 ? lastNetworkEpoch
                : (now - EPOCH_ZERO_MS) / EPOCH_DURATION_MS;
        long epochStartMs = baseEpoch * EPOCH_DURATION_MS + EPOCH_ZERO_MS;
        long msIntoEpoch = now - epochStartMs;
        // Clamp to valid range in case of drift
        if (msIntoEpoch < 0) msIntoEpoch = 0;
        if (msIntoEpoch > EPOCH_DURATION_MS) msIntoEpoch = EPOCH_DURATION_MS;
        int pct = (int) ((msIntoEpoch * 100) / EPOCH_DURATION_MS);
        long secLeft = (EPOCH_DURATION_MS - msIntoEpoch) / 1000;
        if (epochProgress != null) epochProgress.setProgress(pct);
        if (tvEpochProgress != null) tvEpochProgress.setText(secLeft + "s remaining");
    }

    private void applyCard(TextView dot, TextView title, TextView status,
                           boolean enabled, String label, String state) {
        if (dot == null) return;

        int dotColor;
        if (!enabled) {
            dotColor = 0xFF4B5563; // gray — off
        } else if (state.toLowerCase().contains("error") || state.toLowerCase().contains("failed")
                || state.toLowerCase().contains("unavailable")) {
            dotColor = 0xFFEF4444; // red
        } else if (state.toLowerCase().contains("download") || state.toLowerCase().contains("starting")
                || state.toLowerCase().contains("connecting") || state.toLowerCase().contains("loading")) {
            dotColor = 0xFFF7931A; // orange — working
        } else if (enabled) {
            dotColor = 0xFF22C55E; // green — active
        } else {
            dotColor = 0xFF4B5563;
        }

        dot.setTextColor(dotColor);
        if (title != null) title.setText(label);
        if (status != null) status.setText(state);
    }

    // ---- chain stats (from public/network API) ----

    private void fetchChainStats() {
        if (!isAdded()) return;
        String url = prefs.getApiUrl() + "/public/network";
        Request req = new Request.Builder().url(url).get().build();
        http.newCall(req).enqueue(new Callback() {
            @Override public void onFailure(Call call, IOException e) {
                handler.post(() -> {
                    if (!isAdded()) return;
                    if (tvSyncStatus != null) {
                        tvSyncStatus.setText("Network unreachable");
                        tvSyncStatus.setTextColor(0xFFEF4444);
                    }
                });
            }
            @Override public void onResponse(Call call, Response response) throws IOException {
                try (ResponseBody rb = response.body()) {
                    if (!response.isSuccessful() || rb == null) return;
                    JSONObject j = new JSONObject(rb.string());
                    handler.post(() -> applyChainStats(j));
                } catch (Exception ignored) {}
            }
        });
    }

    private void applyChainStats(JSONObject j) {
        if (!isAdded()) return;
        long epoch     = j.optLong("epoch", 0);
        int  peers     = j.optInt("peer_count", 0);
        int  miners    = j.optInt("miners", 0);
        int  clocks    = j.optInt("clocks", 0) + j.optInt("browser_clocks", 0);
        int  nodes     = j.optInt("nodes", 0);
        int  sensors   = j.optInt("sensors", 0);
        int  gateways  = j.optInt("gateways", 0);
        int  verifiers = j.optInt("verifiers", 0);
        boolean alive  = j.optBoolean("alive", false);

        if (epoch > 0) lastNetworkEpoch = epoch;

        if (tvEpoch   != null) tvEpoch.setText(String.valueOf(epoch));
        if (tvPeers   != null) tvPeers.setText(String.valueOf(peers));
        if (tvMiners  != null) tvMiners.setText(String.valueOf(miners));
        if (tvClocks  != null) tvClocks.setText(String.valueOf(clocks));
        if (tvNodes   != null) tvNodes.setText(String.valueOf(nodes));
        if (tvSensorsCount != null) tvSensorsCount.setText(String.valueOf(sensors));
        if (tvGateways   != null) tvGateways.setText(String.valueOf(gateways));
        if (tvVerifiers  != null) tvVerifiers.setText(String.valueOf(verifiers));

        if (tvStatus != null) {
            tvStatus.setText(alive ? "alive" : "stalled");
            tvStatus.setTextColor(alive ? 0xFF22C55E : 0xFFEF4444);
        }

        // Sync status: if we received a network response, the phone is synced.
        // The local wall-clock epoch can differ from the chain epoch due to
        // block finalization lag — use the network response as the authority.
        if (tvSyncStatus != null) {
            tvSyncStatus.setText("Synced");
            tvSyncStatus.setTextColor(0xFF22C55E);
        }
    }

    // ---- node detail bottom sheet ----

    private void fetchAndShowNodeDetail(String nodeType, String title) {
        if (!isAdded()) return;
        String url = prefs.getApiUrl() + "/public/network/nodes";
        Request req = new Request.Builder().url(url).get().build();
        http.newCall(req).enqueue(new Callback() {
            @Override public void onFailure(Call call, IOException e) {
                handler.post(() -> {
                    if (!isAdded()) return;
                    showNodeDialog(title, "Could not reach server.");
                });
            }
            @Override public void onResponse(Call call, Response response) throws IOException {
                String body = "";
                try (ResponseBody rb = response.body()) {
                    if (rb != null) body = rb.string();
                }
                final String finalBody = body;
                handler.post(() -> {
                    if (!isAdded()) return;
                    try {
                        JSONObject root = new JSONObject(finalBody);
                        JSONArray arr = root.optJSONArray(nodeType);
                        if (arr == null || arr.length() == 0) {
                            showNodeDialog(title, "No " + title.toLowerCase() + " found.");
                            return;
                        }
                        StringBuilder sb = new StringBuilder();
                        for (int i = 0; i < arr.length(); i++) {
                            JSONObject node = arr.optJSONObject(i);
                            if (node == null) continue;
                            sb.append(formatNodeEntry(nodeType, node));
                            if (i < arr.length() - 1) sb.append('\n');
                        }
                        showNodeDialog(title, sb.toString());
                    } catch (Exception e) {
                        showNodeDialog(title, "Error parsing response.");
                    }
                });
            }
        });
    }

    private String formatNodeEntry(String nodeType, JSONObject node) {
        switch (nodeType) {
            case "miners": {
                String account = node.optString("account", "?");
                String model   = node.optString("model", "");
                long lastEpoch = node.optLong("last_epoch", 0);
                StringBuilder s = new StringBuilder(account);
                if (!model.isEmpty()) s.append(" — ").append(model);
                if (lastEpoch > 0) s.append(" (ep ").append(lastEpoch).append(')');
                return s.toString();
            }
            case "clocks": {
                String account = node.optString("account", "?");
                String source  = node.optString("source", "");
                return account + (source.isEmpty() ? "" : " [" + source + "]");
            }
            case "sensors": {
                String account = node.optString("account", "?");
                String sensorId = node.optString("sensor_id", "");
                long lastEpoch  = node.optLong("last_reading_epoch", 0);
                int readings    = node.optInt("total_readings", 0);
                StringBuilder s = new StringBuilder(account);
                if (!sensorId.isEmpty() && !sensorId.equals(account)) s.append('/').append(sensorId);
                if (lastEpoch > 0) s.append(" ep ").append(lastEpoch);
                if (readings > 0) s.append(" (").append(readings).append(" readings)");
                return s.toString();
            }
            case "storage": {
                String account = node.optString("account", "?");
                long capacity  = node.optLong("capacity_gb", 0);
                int files      = node.optInt("files_stored", 0);
                StringBuilder s = new StringBuilder(account);
                if (capacity > 0) s.append(" — ").append(capacity).append(" GB");
                if (files > 0) s.append(", ").append(files).append(" files");
                return s.toString();
            }
            case "gateways": {
                String account = node.optString("account", "?");
                String name    = node.optString("name", "");
                String region  = node.optString("region", "");
                StringBuilder s = new StringBuilder(account);
                if (!name.isEmpty()) s.append(" — ").append(name);
                if (!region.isEmpty()) s.append(" (").append(region).append(')');
                return s.toString();
            }
            default: {
                return node.optString("account", node.toString());
            }
        }
    }

    private void showNodeDialog(String title, String message) {
        if (!isAdded()) return;
        new AlertDialog.Builder(requireContext())
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton("OK", null)
                .show();
    }
}
