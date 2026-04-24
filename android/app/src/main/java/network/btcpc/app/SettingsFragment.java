package network.btcpc.app;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.activity.result.ActivityResultLauncher;
import androidx.appcompat.app.AlertDialog;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.core.content.ContextCompat;

import com.journeyapps.barcodescanner.ScanContract;
import com.journeyapps.barcodescanner.ScanOptions;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.chip.Chip;
import com.google.android.material.chip.ChipGroup;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * SettingsFragment — lets the user configure account, JWT, API URL and device name.
 *
 * Service status rows are read from SharedPrefs (written by the background services)
 * and refreshed each time the fragment resumes.
 */
public class SettingsFragment extends Fragment {

    private AppPrefs prefs;

    private TextView accountLabel;
    private MaterialButton signOutBtn;
    private EditText postingKeyInput;
    private EditText apiUrlInput;
    private EditText relayUrlInput;
    private EditText deviceNameInput;
    private EditText wifiSuggestionInput;
    private MaterialButton wifiAddBtn;
    private ChipGroup wifiNearbyChips;
    private TextView wifiPermissionHint;
    private ChipGroup trustedWifiChips;
    private MaterialButton saveBtn;
    private TextView saveStatus;

    private TextView relayStatusView;
    private TextView clockStatusView;
    private TextView sensorStatusView;
    private TextView minerStatusView;
    private TextView storageStatusView;

    private TextView versionView;
    private com.google.android.material.button.MaterialButton updateBtn;
    private TextView updateStatus;

    private final Handler handler = new Handler(Looper.getMainLooper());

    private final ActivityResultLauncher<ScanOptions> qrScanLauncher =
        registerForActivityResult(new ScanContract(), result -> {
            if (result.getContents() == null) return;
            String raw = result.getContents().trim();
            String[] parts = WalletBackupHelper.parsePayload(raw);
            if (parts != null && parts[1].length() == 64) {
                applyWalletRestore(parts);
            } else {
                showRestoreDialogWithText(raw);
            }
        });

    @Override
    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container,
                             Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_settings, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        prefs = new AppPrefs(requireContext());

        accountLabel    = view.findViewById(R.id.settings_account_label);
        signOutBtn      = view.findViewById(R.id.settings_signout_btn);
        postingKeyInput = view.findViewById(R.id.settings_posting_key);
        apiUrlInput     = view.findViewById(R.id.settings_api_url);
        relayUrlInput   = view.findViewById(R.id.settings_relay_url);
        deviceNameInput = view.findViewById(R.id.settings_device_name);
        wifiSuggestionInput  = view.findViewById(R.id.settings_wifi_suggestion);
        wifiAddBtn           = view.findViewById(R.id.settings_wifi_add_btn);
        wifiNearbyChips      = view.findViewById(R.id.settings_wifi_nearby_chips);
        wifiPermissionHint   = view.findViewById(R.id.settings_wifi_permission_hint);
        trustedWifiChips     = view.findViewById(R.id.settings_trusted_wifi_chips);
        saveBtn         = view.findViewById(R.id.settings_save_btn);
        saveStatus      = view.findViewById(R.id.settings_save_status);

        relayStatusView  = view.findViewById(R.id.settings_relay_status);
        clockStatusView  = view.findViewById(R.id.settings_clock_status);
        sensorStatusView = view.findViewById(R.id.settings_sensor_status);
        minerStatusView  = view.findViewById(R.id.settings_miner_status);
        storageStatusView = view.findViewById(R.id.settings_storage_status);

        versionView  = view.findViewById(R.id.settings_version);
        updateBtn    = view.findViewById(R.id.settings_update_btn);
        updateStatus = view.findViewById(R.id.settings_update_status);

        versionView.setText("v" + BuildConfig.VERSION_NAME);
        updateBtn.setOnClickListener(v -> checkForUpdate());

        wifiAddBtn.setOnClickListener(v -> addTrustedWifiFromInput());

        // Show signed-in account or prompt to sign in
        String account = prefs.getAccount();
        if (!account.isEmpty()) {
            accountLabel.setText("Signed in as " + account);
            signOutBtn.setVisibility(android.view.View.VISIBLE);
        } else {
            accountLabel.setText("Not signed in — use the Wallet tab to sign in");
            signOutBtn.setVisibility(android.view.View.GONE);
        }
        signOutBtn.setOnClickListener(v -> {
            prefs.saveAll("", "", "", prefs.getApiUrl(), prefs.getRelayUrl(), prefs.getDeviceName());
            accountLabel.setText("Not signed in — use the Wallet tab to sign in");
            signOutBtn.setVisibility(android.view.View.GONE);
            saveStatus.setText("Signed out.");
            saveStatus.setTextColor(0xFF22C55E);
            handler.postDelayed(() -> { if (isAdded()) saveStatus.setText(""); }, 2000);
        });

        postingKeyInput.setText(prefs.getPostingKey());
        apiUrlInput.setText(prefs.getApiUrl());
        relayUrlInput.setText(prefs.getRelayUrl());
        deviceNameInput.setText(prefs.getDeviceName());

        saveBtn.setOnClickListener(v -> saveSettings());

        view.findViewById(R.id.settings_backup_qr_btn).setOnClickListener(v -> showBackupQr());
        view.findViewById(R.id.settings_restore_btn).setOnClickListener(v -> showRestoreDialog());

        refreshTrustedWifiUi();
        refreshServiceStatuses();
        // Auto-check for updates when settings opens
        checkForUpdate();
    }

    @Override
    public void onResume() {
        super.onResume();
        if (prefs != null && accountLabel != null) {
            String acct = prefs.getAccount();
            if (!acct.isEmpty()) {
                accountLabel.setText("Signed in as " + acct);
                signOutBtn.setVisibility(android.view.View.VISIBLE);
            } else {
                accountLabel.setText("Not signed in — use the Wallet tab to sign in");
                signOutBtn.setVisibility(android.view.View.GONE);
            }
            // Refresh fields that may have been updated by login in another tab
            if (postingKeyInput != null && postingKeyInput.getText().toString().isEmpty()) {
                String saved = prefs.getPostingKey();
                if (!saved.isEmpty()) postingKeyInput.setText(saved);
            }
            if (deviceNameInput != null) {
                String saved = prefs.getDeviceName();
                String current = deviceNameInput.getText().toString();
                if (!saved.equals(current)) deviceNameInput.setText(saved);
            }
        }
        refreshTrustedWifiUi();
        refreshServiceStatuses();
    }

    // ---- save ----

    private void saveSettings() {
        String postingKey = text(postingKeyInput);
        String apiUrl     = text(apiUrlInput);
        String relayUrl   = text(relayUrlInput);
        String deviceName = text(deviceNameInput);

        if (apiUrl.isEmpty())   apiUrl   = AppPrefs.DEFAULT_API_URL;
        if (relayUrl.isEmpty()) relayUrl = AppPrefs.DEFAULT_RELAY_URL;

        if (!postingKey.isEmpty() && !postingKey.matches("[0-9a-fA-F]{64}")) {
            saveStatus.setText("Posting key must be 64 hex characters");
            saveStatus.setTextColor(0xFFEF4444);
            return;
        }

        prefs.saveAll(prefs.getAccount(), prefs.getJwt(), postingKey, apiUrl, relayUrl, deviceName);

        saveStatus.setText("Saved. Add private Wi-Fi names below to keep GPS private at home or work.");
        saveStatus.setTextColor(0xFF22C55E);  // green

        // Clear status text after 2 s
        handler.postDelayed(() -> {
            if (!isAdded()) return;
            saveStatus.setText("");
        }, 2000);
    }

    private String text(EditText et) {
        if (et == null || et.getText() == null) return "";
        return et.getText().toString().trim();
    }

    private void addTrustedWifiFromInput() {
        if (!isAdded() || prefs == null || wifiSuggestionInput == null) return;
        String ssid = TrustedWifiPolicy.normalizeSsid(text(wifiSuggestionInput));
        if (ssid.isEmpty()) return;
        addTrustedWifiFromInput(ssid);
        wifiSuggestionInput.setText("");
    }

    private void addTrustedWifiFromInput(String ssid) {
        if (!isAdded() || prefs == null) return;
        ssid = TrustedWifiPolicy.normalizeSsid(ssid);
        if (ssid.isEmpty()) return;
        prefs.addTrustedWifiSsid(ssid);
        refreshTrustedWifiUi();
        saveStatus.setText("Added " + ssid + " to privacy list.");
        saveStatus.setTextColor(0xFF22C55E);
        handler.postDelayed(() -> {
            if (isAdded() && saveStatus != null) saveStatus.setText("");
        }, 2000);
    }

    private void refreshTrustedWifiUi() {
        if (!isAdded() || prefs == null) return;
        renderTrustedWifiChips();
        refreshWifiSuggestions();
    }

    private void renderTrustedWifiChips() {
        if (trustedWifiChips == null) return;
        trustedWifiChips.removeAllViews();

        Set<String> trusted = prefs.getTrustedWifiSsidSet();
        if (trusted.isEmpty()) {
            Chip empty = new Chip(requireContext());
            empty.setText("No PRIVATE Wi-Fi added");
            empty.setCheckable(false);
            empty.setClickable(false);
            empty.setCloseIconVisible(false);
            empty.setEnabled(false);
            trustedWifiChips.addView(empty);
            return;
        }

        for (String ssid : trusted) {
            if (ssid == null || ssid.trim().isEmpty()) continue;
            Chip chip = new Chip(requireContext());
            chip.setText(ssid);
            chip.setChipBackgroundColorResource(android.R.color.transparent);
            chip.setCloseIconVisible(true);
            chip.setCheckable(false);
            chip.setOnClickListener(v -> wifiSuggestionInput.setText(ssid));
            chip.setOnCloseIconClickListener(v -> {
                prefs.removeTrustedWifiSsid(ssid);
                refreshTrustedWifiUi();
            });
            trustedWifiChips.addView(chip);
        }
    }

    private void refreshWifiSuggestions() {
        if (!isAdded()) return;
        boolean hasPerm = hasWifiScanPermission();
        if (wifiPermissionHint != null) {
            wifiPermissionHint.setVisibility(hasPerm ? View.GONE : View.VISIBLE);
            if (!hasPerm) {
                wifiPermissionHint.setOnClickListener(v -> requestPermissions(
                        new String[]{Manifest.permission.ACCESS_FINE_LOCATION}, 0));
            }
        }
        if (wifiNearbyChips == null) return;
        wifiNearbyChips.removeAllViews();
        List<String> visible = loadVisibleWifiSsids();
        Set<String> trusted = prefs.getTrustedWifiSsidSet();
        if (visible.isEmpty()) {
            Chip placeholder = new Chip(requireContext());
            placeholder.setText(hasPerm ? "No networks found" : "Grant location to scan");
            placeholder.setCheckable(false);
            placeholder.setClickable(false);
            placeholder.setEnabled(false);
            wifiNearbyChips.addView(placeholder);
            return;
        }
        for (String ssid : visible) {
            Chip chip = new Chip(requireContext());
            chip.setText(ssid);
            chip.setCheckable(false);
            chip.setCloseIconVisible(false);
            if (trusted.contains(ssid)) {
                chip.setChipBackgroundColorResource(R.color.btcpc_surface_alt);
            }
            chip.setOnClickListener(v -> addTrustedWifiFromInput(ssid));
            wifiNearbyChips.addView(chip);
        }
    }

    private List<String> loadVisibleWifiSsids() {
        LinkedHashSet<String> ssids = new LinkedHashSet<>();
        String connected = currentWifiSsid();
        if (!connected.isEmpty()) ssids.add(connected);

        if (hasWifiScanPermission()) {
            try {
                WifiManager wifiManager = (WifiManager) requireContext().getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wifiManager != null) {
                    List<ScanResult> results = wifiManager.getScanResults();
                    if (results != null) {
                        for (ScanResult result : results) {
                            String ssid = TrustedWifiPolicy.normalizeSsid(result != null ? result.SSID : null);
                            if (!ssid.isEmpty()) ssids.add(ssid);
                        }
                    }
                }
            } catch (Exception ignored) {}
        }

        ArrayList<String> out = new ArrayList<>(ssids);
        Collections.sort(out, String.CASE_INSENSITIVE_ORDER);
        return out;
    }

    private String currentWifiSsid() {
        try {
            ConnectivityManager cm = (ConnectivityManager) requireContext().getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) return "";
            Network active = cm.getActiveNetwork();
            if (active == null) return "";
            NetworkCapabilities caps = cm.getNetworkCapabilities(active);
            if (caps == null || !caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return "";
            WifiManager wifiManager = (WifiManager) requireContext().getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifiManager == null) return "";
            WifiInfo info = wifiManager.getConnectionInfo();
            return TrustedWifiPolicy.normalizeSsid(info != null ? info.getSSID() : null);
        } catch (Exception ignored) {
            return "";
        }
    }

    private boolean hasWifiScanPermission() {
        boolean fine = ContextCompat.checkSelfPermission(requireContext(), Manifest.permission.ACCESS_FINE_LOCATION)
                == PackageManager.PERMISSION_GRANTED;
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
            boolean nearby = ContextCompat.checkSelfPermission(requireContext(), Manifest.permission.NEARBY_WIFI_DEVICES)
                    == PackageManager.PERMISSION_GRANTED;
            return fine || nearby;
        }
        return fine;
    }

    // ---- update check ----

    private void checkForUpdate() {
        if (!isAdded()) return;
        updateStatus.setText("Checking for updates…");
        updateStatus.setVisibility(android.view.View.VISIBLE);
        updateBtn.setEnabled(false);

        UpdateChecker.check(requireContext(), prefs.getApiUrl(), new UpdateChecker.Listener() {
            @Override
            public void onUpdateAvailable(String versionName, String changelog, Runnable install) {
                if (!isAdded()) return;
                updateStatus.setText("Update available: v" + versionName);
                updateStatus.setTextColor(0xFFF7931A); // orange
                updateBtn.setText("Update to v" + versionName);
                updateBtn.setEnabled(true);
                updateBtn.setOnClickListener(v -> {
                    updateBtn.setEnabled(false);
                    updateStatus.setText("Downloading…");
                    updateStatus.setTextColor(0xFFA8B0BF); // muted
                    install.run();
                });
            }

            @Override
            public void onUpToDate(String currentVersion) {
                if (!isAdded()) return;
                updateStatus.setText("Up to date (v" + currentVersion + ")");
                updateStatus.setTextColor(0xFF22C55E); // green
                updateBtn.setText("Check for updates");
                updateBtn.setEnabled(true);
                updateBtn.setOnClickListener(v2 -> checkForUpdate());
                handler.postDelayed(() -> {
                    if (isAdded()) updateStatus.setVisibility(android.view.View.GONE);
                }, 3000);
            }

            @Override
            public void onError(String msg) {
                if (!isAdded()) return;
                updateStatus.setText("Update check failed");
                updateStatus.setTextColor(0xFFA8B0BF);
                updateBtn.setText("Check for updates");
                updateBtn.setEnabled(true);
                updateBtn.setOnClickListener(v2 -> checkForUpdate());
            }
        });
    }

    // ---- service status ----

    private void refreshServiceStatuses() {
        if (!isAdded() || prefs == null) return;

        String relay  = prefs.getRelayState();
        String clock  = prefs.getClockState();
        String sensor = prefs.getSensorState();
        String miner  = prefs.getMinerState();
        String storage = prefs.getStorageState();

        applyProcessStatus(relayStatusView, relay);
        applyProcessStatus(clockStatusView, clock);
        applyProcessStatus(sensorStatusView, sensor);
        applyProcessStatus(minerStatusView, miner);
        applyProcessStatus(storageStatusView, storage);
    }

    private void applyProcessStatus(TextView statusView, String state) {
        if (statusView == null) return;
        String value = (state == null || state.trim().isEmpty()) ? "Stopped" : state.trim();
        int color;
        if (isHealthy(value)) {
            color = 0xFF22C55E;
        } else if (isTransitioning(value)) {
            color = 0xFFF7931A;
        } else if (isError(value)) {
            color = 0xFFEF4444;
        } else {
            color = 0xFFA8B0BF;
        }
        statusView.setText("● " + value);
        statusView.setTextColor(color);
    }

    private static boolean isHealthy(String s) {
        String lower = s.toLowerCase();
        return lower.contains("running") || lower.contains("active") || lower.contains("connected")
                || lower.contains("serving") || lower.contains("hosting") || lower.contains("proof submitted")
                || lower.contains("mining with") || lower.contains("inference");
    }

    private static boolean isTransitioning(String s) {
        String lower = s.toLowerCase();
        return lower.contains("starting") || lower.contains("downloading") || lower.contains("connecting")
                || lower.contains("loading") || lower.contains("assembling");
    }

    private static boolean isError(String s) {
        String lower = s.toLowerCase();
        return lower.contains("error") || lower.contains("failed") || lower.contains("unavailable");
    }

    // ---- backup / restore ----

    private void showBackupQr() {
        if (!isAdded()) return;
        String account = prefs.getAccount();
        String postingKey = prefs.getPostingKey();
        if (account.isEmpty() || postingKey.isEmpty()) {
            saveStatus.setText("Sign in and save your posting key first");
            saveStatus.setTextColor(0xFFEF4444);
            handler.postDelayed(() -> { if (isAdded()) saveStatus.setText(""); }, 3000);
            return;
        }
        int sizePx = (int) (280 * getResources().getDisplayMetrics().density);
        Bitmap qr = WalletBackupHelper.generateQr(account, postingKey, sizePx);
        if (qr == null) {
            saveStatus.setText("Failed to generate QR");
            saveStatus.setTextColor(0xFFEF4444);
            return;
        }
        android.view.View dialogView = LayoutInflater.from(requireContext())
                .inflate(R.layout.dialog_backup_qr, null, false);
        ImageView qrView = dialogView.findViewById(R.id.backup_qr_image);
        TextView labelView = dialogView.findViewById(R.id.backup_qr_label);
        qrView.setImageBitmap(qr);
        labelView.setText("Screenshot this QR. On a new phone: Settings → Restore Wallet → scan or paste.");
        new AlertDialog.Builder(requireContext())
                .setTitle("Wallet Backup QR")
                .setView(dialogView)
                .setPositiveButton("Done", null)
                .show();
    }

    private void showRestoreDialog() {
        showRestoreDialogWithText("");
    }

    private void showRestoreDialogWithText(String prefill) {
        if (!isAdded()) return;
        android.view.View dialogView = LayoutInflater.from(requireContext())
                .inflate(R.layout.dialog_restore_wallet, null, false);
        EditText input = dialogView.findViewById(R.id.restore_input);
        if (!prefill.isEmpty()) input.setText(prefill);
        com.google.android.material.button.MaterialButton scanBtn =
                dialogView.findViewById(R.id.restore_scan_btn);

        AlertDialog dialog = new AlertDialog.Builder(requireContext())
                .setTitle("Restore Wallet")
                .setMessage("Paste your backup code or scan the backup QR.")
                .setView(dialogView)
                .setPositiveButton("Restore", (d, which) -> {
                    String raw = input.getText() != null ? input.getText().toString().trim() : "";
                    String[] parts = WalletBackupHelper.parsePayload(raw);
                    if (parts == null || parts[1].length() != 64) {
                        saveStatus.setText("Invalid backup code. Format: account:64hexkey");
                        saveStatus.setTextColor(0xFFEF4444);
                        handler.postDelayed(() -> { if (isAdded()) saveStatus.setText(""); }, 4000);
                        return;
                    }
                    applyWalletRestore(parts);
                })
                .setNegativeButton("Cancel", null)
                .create();

        scanBtn.setOnClickListener(v -> {
            dialog.dismiss();
            ScanOptions opts = new ScanOptions()
                    .setPrompt("Scan your BTCPC wallet backup QR")
                    .setBeepEnabled(false)
                    .setOrientationLocked(false)
                    .setBarcodeImageEnabled(false);
            qrScanLauncher.launch(opts);
        });

        dialog.show();
    }

    private void applyWalletRestore(String[] parts) {
        prefs.setAccount(parts[0]);
        prefs.setPostingKey(parts[1]);
        accountLabel.setText("Signed in as " + parts[0]);
        signOutBtn.setVisibility(android.view.View.VISIBLE);
        postingKeyInput.setText(parts[1]);
        saveStatus.setText("Wallet restored for " + parts[0]);
        saveStatus.setTextColor(0xFF22C55E);
        handler.postDelayed(() -> { if (isAdded()) saveStatus.setText(""); }, 3000);
    }
}
