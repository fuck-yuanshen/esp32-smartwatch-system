package com.example.ble;

import android.annotation.SuppressLint;
import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.provider.OpenableColumns;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Queue;
import java.util.UUID;

@SuppressLint({"SetTextI18n", "MissingPermission", "SpellCheckingInspection"})
public class MainActivity extends AppCompatActivity {
    private static final String DEVICE_NAME = "BBWF WATCH";
    // 小分块可以降低 ESP32 WebServer 内存压力；大文件上传中途断网时，重试成本也更低。
    private static final int LAN_UPLOAD_CHUNK_SIZE = 2048;
    private static final int LAN_UPLOAD_MAX_RETRIES = 90;
    private static final UUID SERVICE_UUID = UUID.fromString("6f4d0001-6f6f-4f20-8677-626277660001");
    private static final UUID STATUS_UUID = UUID.fromString("6f4d0002-6f6f-4f20-8677-626277660001");
    private static final UUID COMMAND_UUID = UUID.fromString("6f4d0003-6f6f-4f20-8677-626277660001");
    private static final UUID CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final int REQUEST_PERMISSIONS = 7;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Queue<String> pendingCommands = new ArrayDeque<>();
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothDevice pendingDevice;
    private BluetoothGattCharacteristic commandCharacteristic;
    private boolean writeInProgress = false;
    private boolean retryingConnection = false;
    private TextView statusText;
    private TextView watchStatusText;
    private TextView selectedBookText;
    private EditText ssidEdit;
    private EditText passEdit;
    private EditText alarmEdit;
    private EditText alarmIdEdit;
    private EditText bookIdEdit;
    private EditText lanIpEdit;
    private Uri selectedBookUri;
    private String selectedBookName = "";
    private String lanIp = "";

    private final ActivityResultLauncher<Intent> pickTxtLauncher =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() != RESULT_OK || result.getData() == null) return;
                selectedBookUri = result.getData().getData();
                selectedBookName = queryFileName(selectedBookUri);
                selectedBookText.setText("已选择：" + selectedBookName);
            });

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            handleScanResult(result);
        }

        @Override
        public void onBatchScanResults(List<ScanResult> results) {
            for (ScanResult result : results) handleScanResult(result);
        }

        @Override
        public void onScanFailed(int errorCode) {
            setStatus("扫描失败：" + errorCode);
        }
    };

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                setStatus("已连接，正在读取服务");
                retryingConnection = false;
                if (hasBluetoothPermission()) {
                    gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
                    gatt.requestMtu(247);
                    handler.postDelayed(gatt::discoverServices, 500);
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                commandCharacteristic = null;
                closeGatt(false);
                if (!retryingConnection && pendingDevice != null) {
                    retryingConnection = true;
                    setStatus("连接断开，正在重试");
                    handler.postDelayed(() -> connectDevice(pendingDevice), 700);
                } else {
                    setStatus("已断开");
                }
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                setStatus("读取服务失败：" + status);
                return;
            }
            BluetoothGattService service = gatt.getService(SERVICE_UUID);
            if (service == null) {
                setStatus("未找到手表配置服务");
                return;
            }
            commandCharacteristic = service.getCharacteristic(COMMAND_UUID);
            BluetoothGattCharacteristic statusCharacteristic = service.getCharacteristic(STATUS_UUID);
            if (hasBluetoothPermission() && statusCharacteristic != null) {
                gatt.setCharacteristicNotification(statusCharacteristic, true);
                BluetoothGattDescriptor descriptor = statusCharacteristic.getDescriptor(CCCD_UUID);
                if (descriptor != null) {
                    descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    gatt.writeDescriptor(descriptor);
                }
                gatt.readCharacteristic(statusCharacteristic);
            }
            setStatus("已连接 BBWF Watch");
        }

        @Override
        public void onCharacteristicRead(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            showWatchStatus(characteristic.getStringValue(0));
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
            showWatchStatus(characteristic.getStringValue(0));
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            writeInProgress = false;
            handler.post(MainActivity.this::writeNextCommand);
        }
    };
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusText = findViewById(R.id.statusText);
        watchStatusText = findViewById(R.id.watchStatusText);
        selectedBookText = findViewById(R.id.selectedBookText);
        ssidEdit = findViewById(R.id.ssidEdit);
        passEdit = findViewById(R.id.passEdit);
        alarmEdit = findViewById(R.id.alarmEdit);
        alarmIdEdit = findViewById(R.id.alarmIdEdit);
        bookIdEdit = findViewById(R.id.bookIdEdit);
        lanIpEdit = findViewById(R.id.lanIpEdit);

        Button scanButton = findViewById(R.id.scanButton);
        Button refreshButton = findViewById(R.id.refreshButton);
        Button setPhoneTimeButton = findViewById(R.id.setPhoneTimeButton);
        Button saveWifiButton = findViewById(R.id.saveWifiButton);
        Button addAlarmButton = findViewById(R.id.addAlarmButton);
        Button toggleAlarmButton = findViewById(R.id.toggleAlarmButton);
        Button deleteAlarmButton = findViewById(R.id.deleteAlarmButton);
        Button selectBookButton = findViewById(R.id.selectBookButton);
        Button uploadBookButton = findViewById(R.id.uploadBookButton);
        Button useLanIpButton = findViewById(R.id.useLanIpButton);
        Button deleteBookButton = findViewById(R.id.deleteBookButton);

        BluetoothManager manager = getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        scanner = adapter == null ? null : adapter.getBluetoothLeScanner();

        scanButton.setOnClickListener(v -> startScan());
        refreshButton.setOnClickListener(v -> sendCommand("BOOK_REFRESH"));
        setPhoneTimeButton.setOnClickListener(v -> sendCommand("SET_TIME:" + phoneTimeText()));
        saveWifiButton.setOnClickListener(v -> sendCommand("WIFI:" + ssidEdit.getText() + "|" + passEdit.getText()));
        addAlarmButton.setOnClickListener(v -> sendCommand("ALARM_ADD:" + alarmEdit.getText()));
        toggleAlarmButton.setOnClickListener(v -> sendCommand("ALARM_TOGGLE:" + alarmIdEdit.getText()));
        deleteAlarmButton.setOnClickListener(v -> sendCommand("ALARM_DELETE:" + alarmIdEdit.getText()));
        selectBookButton.setOnClickListener(v -> selectBook());
        uploadBookButton.setOnClickListener(v -> uploadSelectedBook());
        useLanIpButton.setOnClickListener(v -> {
            if (lanIp != null && !lanIp.isEmpty()) lanIpEdit.setText(lanIp);
        });
        deleteBookButton.setOnClickListener(v -> sendCommand("BOOK_DELETE:" + bookIdEdit.getText()));

        ensurePermissions();
    }

    private void selectBook() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("text/*");
        pickTxtLauncher.launch(intent);
    }

    private void uploadSelectedBook() {
        if (selectedBookUri == null) {
            Toast.makeText(this, "请先选择 TXT 小说", Toast.LENGTH_SHORT).show();
            return;
        }
        String targetIp = lanIpEdit.getText().toString().trim();
        if (targetIp.isEmpty()) targetIp = lanIp == null ? "" : lanIp.trim();
        if (targetIp.isEmpty()) {
            Toast.makeText(this, "请先保存 WiFi，或手动输入手表局域网 IP", Toast.LENGTH_LONG).show();
            sendCommand("LAN_CONNECT");
            return;
        }
        lanIp = targetIp;
        lanIpEdit.setText(targetIp);
        try (InputStream input = getContentResolver().openInputStream(selectedBookUri)) {
            if (input == null) throw new IllegalStateException("无法读取文件");
            // 手表阅读器用 HZK16/GBK 点阵字库显示中文，所以 BLE 和局域网上传前都先转成 GBK。
            byte[] data = toGbkNovelBytes(readAllBytes(input));
            if (data.length == 0) throw new IllegalStateException("文件为空");
            selectedBookName = normalizeBookName(selectedBookName);
            if (!selectedBookName.toLowerCase(Locale.ROOT).endsWith(".txt")) selectedBookName += ".txt";
            String uploadName = selectedBookName;
            setStatus("正在通过局域网上传：" + uploadName + "，大小 " + data.length + " 字节");
            String uploadIp = targetIp;
            new Thread(() -> uploadBookByHttp(uploadIp, uploadName, data)).start();
        } catch (Exception e) {
            Toast.makeText(this, "上传失败：" + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private byte[] readAllBytes(InputStream input) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int read;
        while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
        return output.toByteArray();
    }

    private void uploadBookByHttp(String ip, String fileName, byte[] data) {
        String remoteName = null;
        int offset = 0;
        int retries = 0;
        try {
            // 让手表分配 SD 卡里的真实文件名，避免 Android 文件选择器只返回 ".txt" 之类的异常名字。
            JSONObject begin = httpJson("http://" + ip + "/book/chunk/begin?file=" + urlEncode(fileName), null);
            remoteName = begin.optString("name", "");
            offset = begin.optInt("offset", 0);
            if (remoteName.isEmpty()) throw new IllegalStateException("watch did not return file name");

            while (offset < data.length) {
                int count = Math.min(LAN_UPLOAD_CHUNK_SIZE, data.length - offset);
                byte[] chunk = new byte[count];
                System.arraycopy(data, offset, chunk, 0, count);
                try {
                    // offset 是分块写入协议的一部分；如果重试可能重复写入，手表会返回 SD 实际 offset。
                    String url = "http://" + ip + "/book/chunk/write?name=" + urlEncode(remoteName) + "&offset=" + offset;
                    JSONObject result = httpJson(url, chunk);
                    int nextOffset = result.optInt("offset", -1);
                    if (nextOffset <= offset) throw new IllegalStateException("offset " + nextOffset);
                    offset = nextOffset;
                    retries = 0;
                    int percent = Math.min(100, (int) ((offset * 100L) / data.length));
                    setStatus("局域网上传 " + percent + "% (" + offset + "/" + data.length + ")");
                } catch (Exception chunkError) {
                    retries++;
                    if (retries > LAN_UPLOAD_MAX_RETRIES) throw chunkError;
                    setStatus("网络中断，等待重连后继续：" + offset + "/" + data.length);
                    Thread.sleep(2000);
                    try {
                        // 重连后查询手表已保存进度；如果查询也失败，就保留上次 offset 等下一轮重试。
                        JSONObject status = httpJson("http://" + ip + "/book/chunk/status?name=" + urlEncode(remoteName), null);
                        offset = Math.max(0, Math.min(status.optInt("offset", offset), data.length));
                    } catch (Exception ignored) {
                    }
                }
            }

            httpJson("http://" + ip + "/book/chunk/finish?name=" + urlEncode(remoteName), null);
            setStatus("小说已通过局域网上传完成：" + remoteName);
            sendCommand("BOOK_REFRESH");
        } catch (Exception e) {
            setStatus("局域网上传失败，已保存进度 " + offset + " 字节：" + e.getMessage());
        }
    }

    private JSONObject httpJson(String urlText, byte[] body) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL(urlText).openConnection();
        conn.setConnectTimeout(8000);
        conn.setReadTimeout(15000);
        conn.setRequestMethod(body == null ? "GET" : "POST");
        if (body != null) {
            conn.setDoOutput(true);
            conn.setRequestProperty("Content-Type", "application/octet-stream");
            // 使用固定长度传输，避免 chunked-transfer 对 ESP32 WebServer 原始二进制请求不稳定。
            conn.setFixedLengthStreamingMode(body.length);
            try (OutputStream out = conn.getOutputStream()) {
                out.write(body);
            }
        }
        int code = conn.getResponseCode();
        InputStream input = code >= 400 ? conn.getErrorStream() : conn.getInputStream();
        String text = input == null ? "" : new String(readAllBytes(input), StandardCharsets.UTF_8);
        conn.disconnect();
        if (code < 200 || code >= 300) throw new IllegalStateException("HTTP " + code + " " + text);
        return text.isEmpty() ? new JSONObject() : new JSONObject(text);
    }

    private String urlEncode(String text) throws Exception {
        return URLEncoder.encode(text == null ? "" : text, "UTF-8");
    }

    private byte[] toGbkNovelBytes(byte[] raw) {
        // 优先按 BOM 判断编码，再用内容校验兜底；已是 GBK/ANSI 的 TXT 会原样返回。
        if (raw.length >= 3 && (raw[0] & 0xFF) == 0xEF && (raw[1] & 0xFF) == 0xBB && (raw[2] & 0xFF) == 0xBF) {
            String text = new String(raw, 3, raw.length - 3, StandardCharsets.UTF_8);
            return text.getBytes(Charset.forName("GBK"));
        }
        if (raw.length >= 2 && (raw[0] & 0xFF) == 0xFF && (raw[1] & 0xFF) == 0xFE) {
            String text = new String(raw, 2, raw.length - 2, StandardCharsets.UTF_16LE);
            return text.getBytes(Charset.forName("GBK"));
        }
        if (raw.length >= 2 && (raw[0] & 0xFF) == 0xFE && (raw[1] & 0xFF) == 0xFF) {
            String text = new String(raw, 2, raw.length - 2, StandardCharsets.UTF_16BE);
            return text.getBytes(Charset.forName("GBK"));
        }
        if (looksLikeUtf8(raw)) {
            return new String(raw, StandardCharsets.UTF_8).getBytes(Charset.forName("GBK"));
        }
        if (looksLikeUtf16Le(raw)) {
            return new String(raw, StandardCharsets.UTF_16LE).getBytes(Charset.forName("GBK"));
        }
        return raw;
    }

    private boolean looksLikeUtf16Le(byte[] data) {
        int pairs = Math.min(data.length / 2, 4096);
        if (pairs < 16) return false;
        int zeroHigh = 0;
        int printableLow = 0;
        for (int i = 0; i < pairs * 2; i += 2) {
            int low = data[i] & 0xFF;
            int high = data[i + 1] & 0xFF;
            if (high == 0) zeroHigh++;
            if (low == 9 || low == 10 || low == 13 || (low >= 32 && low <= 126)) printableLow++;
        }
        return zeroHigh > pairs * 7 / 10 && printableLow > pairs / 2;
    }

    private boolean looksLikeUtf8(byte[] data) {
        boolean hasMultibyte = false;
        // 检查整个文件而不是只看开头；很多小说前面是很长的英文/数字目录，中文在后面才出现。
        for (int i = 0; i < data.length;) {
            int c = data[i] & 0xFF;
            if (c < 0x80) {
                i++;
                continue;
            }
            int width;
            if ((c & 0xE0) == 0xC0) width = 2;
            else if ((c & 0xF0) == 0xE0) width = 3;
            else if ((c & 0xF8) == 0xF0) width = 4;
            else {
                return false;
            }
            if (i + width > data.length) return false;
            for (int j = 1; j < width; j++) {
                if ((data[i + j] & 0xC0) != 0x80) {
                    return false;
                }
            }
            hasMultibyte = true;
            i += width;
        }
        return hasMultibyte || data.length > 0;
    }

    private String queryFileName(Uri uri) {
        String fallback = "book.txt";
        if (uri == null) return fallback;
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    String name = cursor.getString(index);
                    if (name != null && !name.isEmpty()) return name;
                }
            }
        } catch (Exception ignored) {
        }
        return fallback;
    }

    private String normalizeBookName(String name) {
        if (name == null) return "book.txt";
        String clean = name.replace("\\", "/").trim();
        int slash = clean.lastIndexOf('/');
        if (slash >= 0) clean = clean.substring(slash + 1);
        if (clean.isEmpty() || clean.equals(".txt") || clean.equalsIgnoreCase("txt")) return "book.txt";
        return clean;
    }

    private void startScan() {
        if (!ensurePermissions()) return;
        if (scanner == null) {
            setStatus("蓝牙不可用");
            return;
        }
        closeGatt(true);
        pendingDevice = null;
        retryingConnection = false;
        setStatus("正在扫描 BBWF Watch");
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        scanner.startScan(null, settings, scanCallback);
        handler.postDelayed(() -> {
            stopScan();
            if (pendingDevice == null && commandCharacteristic == null) setStatus("未找到手表，请确认手表停在 BLE Config 页面");
        }, 15000);
    }

    private void handleScanResult(ScanResult result) {
        if (!hasBluetoothPermission()) return;
        BluetoothDevice device = result.getDevice();
        String deviceName = device.getName();
        String recordName = result.getScanRecord() == null ? null : result.getScanRecord().getDeviceName();
        boolean hasService = false;
        if (result.getScanRecord() != null && result.getScanRecord().getServiceUuids() != null) {
            hasService = result.getScanRecord().getServiceUuids().contains(new ParcelUuid(SERVICE_UUID));
        }
        boolean nameMatches = matchesWatchName(deviceName) || matchesWatchName(recordName);
        if (!hasService && !nameMatches) return;

        stopScan();
        pendingDevice = device;
        setStatus("找到手表：" + readableName(deviceName, recordName) + "，正在连接");
        connectDevice(device);
    }

    private boolean matchesWatchName(String name) {
        if (name == null) return false;
        String normalized = name.replace(" ", "").toUpperCase(Locale.ROOT);
        return normalized.contains("BBWFWATCH") || normalized.contains("BBWF");
    }

    private String readableName(String deviceName, String recordName) {
        if (recordName != null && !recordName.isEmpty()) return recordName;
        if (deviceName != null && !deviceName.isEmpty()) return deviceName;
        return DEVICE_NAME;
    }

    private void connectDevice(BluetoothDevice device) {
        if (!hasBluetoothPermission()) return;
        closeGatt(false);
        gatt = device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
    }

    private void stopScan() {
        if (scanner != null && hasBluetoothPermission()) scanner.stopScan(scanCallback);
    }

    private void closeGatt(boolean clearDevice) {
        if (gatt != null && hasBluetoothPermission()) {
            gatt.disconnect();
            gatt.close();
        }
        gatt = null;
        commandCharacteristic = null;
        writeInProgress = false;
        pendingCommands.clear();
        if (clearDevice) pendingDevice = null;
    }

    private void sendCommand(String command) {
        if (!hasBluetoothPermission() || gatt == null || commandCharacteristic == null) {
            Toast.makeText(this, "请先连接手表", Toast.LENGTH_SHORT).show();
            return;
        }
        pendingCommands.add(command);
        writeNextCommand();
    }

    private void writeNextCommand() {
        if (writeInProgress || pendingCommands.isEmpty()) return;
        if (!hasBluetoothPermission() || gatt == null || commandCharacteristic == null) return;
        String command = pendingCommands.poll();
        if (command == null) return;
        commandCharacteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        commandCharacteristic.setValue(command.getBytes(StandardCharsets.UTF_8));
        writeInProgress = gatt.writeCharacteristic(commandCharacteristic);
        setStatus(writeInProgress ? "已发送：" + command : "发送失败：" + command);
    }

    private String phoneTimeText() {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault()).format(new Date());
    }

    private boolean ensurePermissions() {
        if (hasBluetoothPermission()) return true;
        ActivityCompat.requestPermissions(this,
                new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT},
                REQUEST_PERMISSIONS);
        return false;
    }

    private boolean hasBluetoothPermission() {
        return ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
                && ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_PERMISSIONS && hasBluetoothPermission()) setStatus("权限已允许");
    }

    private void setStatus(String text) {
        handler.post(() -> statusText.setText(text));
    }

    private void showWatchStatus(String text) {
        handler.post(() -> watchStatusText.setText(formatWatchStatus(text)));
    }

    private String formatWatchStatus(String text) {
        if (text == null || text.isEmpty()) return "暂无状态数据";
        try {
            JSONObject root = new JSONObject(text);
            lanIp = root.optString("lanIp", lanIp == null ? "" : lanIp);
            if (lanIpEdit != null && lanIp != null && !lanIp.isEmpty()) lanIpEdit.setText(lanIp);
            StringBuilder out = new StringBuilder();
            out.append("时间：").append(root.optString("time", "--")).append('\n');
            out.append("心率：").append(root.optInt("heartRate", 0)).append("  血氧：").append(root.optInt("spo2", 0)).append('\n');
            out.append("WiFi：").append(root.optString("wifiSsid", "未设置")).append('\n');
            out.append("WiFi 已连接：").append(root.optBoolean("wifiConnected", false) ? "是" : "否").append('\n');
            out.append("局域网 IP：").append(lanIp == null || lanIp.isEmpty() ? "--" : lanIp).append('\n');
            out.append("小说数量：").append(root.optInt("bookCount", 0)).append('\n');
            out.append("闹钟数量：").append(root.optInt("alarmCount", 0)).append('\n');
            out.append("消息：").append(root.optString("message", ""));
            return out.toString();
        } catch (Exception ignored) {
            return text;
        }
    }

    @Override
    protected void onDestroy() {
        stopScan();
        closeGatt(true);
        super.onDestroy();
    }
}
