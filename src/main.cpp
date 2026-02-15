#ifdef USE_LCD
  #include <M5StickC.h>
#else
  #include <M5Atom.h>
#endif
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "driver/twai.h" // ESP32のCAN(TWAI)ドライバ

#ifdef USE_LCD
  const char *ssid = "M5StickC-Server";
#else
  const char *ssid = "M5Atom-Server";
#endif
const char *password = "12345678";
const char *filename = "/uploaded.bin";
const size_t CHUNK_SIZE = 16; 

const char* config_path = "/config.json";

// CAN送信タイミング設定
uint32_t CAN_PACKET_GAP = 100;
uint32_t CAN_CHUNK_INTERVAL = 100;
uint32_t CAN_SEND_ID1 = 0x123;
uint32_t CAN_SEND_ID2 = 0x124;

// CANピン設定
// platformio.iniから渡されたピン番号を使用
const gpio_num_t CAN_TX_PIN = (gpio_num_t)CAN_TX;
const gpio_num_t CAN_RX_PIN = (gpio_num_t)CAN_RX;

WebServer server(80);
size_t total_file_size = 0;
size_t lastPercent = 0;
bool isUploading = false; // アップロード中フラグ

// CANの初期化
void setupCAN() {
    // モードを TWAI_MODE_NO_ACK に変更
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NO_ACK);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS(); // 500kbps
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        twai_start();
        Serial.println("CAN Driver started in NO_ACK mode");
    }
}

// CAN送信処理 (8byteずつ送信) (ノーアック・モード対応)
void sendCAN(uint32_t id, uint8_t* data, uint8_t len) {
    twai_message_t message;
    message.identifier = id;
    message.extd = 0;           // 標準ID(11bit)
    message.rtr = 0;            // データフレーム
    message.ss = 1;             // Single Shot送信 (再送しない設定: NO_ACK時推奨)
    message.self = 0;           
    message.dlc_non_comp = 0;
    message.data_length_code = len;
    
    // フラグに「ACKを期待しない」設定を明示（ドライバ内部でNO_ACKモードと連動します）
    message.flags = TWAI_MSG_FLAG_NONE; 

    for (int i = 0; i < len; i++) {
        message.data[i] = data[i];
    }

    // 第2引数のタイムアウトを少し長めに取るか、即時送信(0)にします
    if (twai_transmit(&message, pdMS_TO_TICKS(10)) != ESP_OK) {
        Serial.println("CAN Transmit failed (Check if driver is started)");
    } else {
        // Serial.printf("Sent ID: 0x%03X\n", id); // デバッグ用
    }
}

// Web画面の表示
void handleRoot() {
    
    // uploadされたファイルの情報を表示
    String fileStatus = "ファイルはありません";
    if (LittleFS.exists(filename)) {
        File f = LittleFS.open(filename, "r");
        if (f) {
            size_t s = f.size();
            fileStatus = "<b>保存済みファイル:</b> " + String(filename) + "<br>" +
                         "<b>サイズ:</b> " + String(s) + " bytes";
            f.close();
        }
    }

    // 設定変更フォームの生成
    String configHtml = "";
     if (LittleFS.exists(config_path)) {
        File f = LittleFS.open(config_path, "r");
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, f);
        f.close();
        if (error) {
            // パース失敗時はエラーメッセージを表示
            configHtml = "<p style='color:red;'>設定の読み込みに失敗しました。初期状態にリセットしてください。</p>";
        } else {
            configHtml += "<h3>設定変更</h3><form method='POST' action='/save_config'>";
            
            // JSONの全要素をループで取り出し
            JsonObject obj = doc.as<JsonObject>();
            for (JsonPair p : obj) {
                String key = p.key().c_str();
                String value = p.value().as<String>();
                String note = key.indexOf("hex") != -1 ? " (16進数)" : ""; // キー名にhexが含まれば注釈
                
                configHtml += "<div>" + key + note + ": <br>";
                configHtml += "<input type='text' name='" + key + "' value='" + value + "'></div>";
            }
            configHtml += "<input type='submit' value='設定を更新' style='background:#007bff; color:white; padding:10px 20px; border:none; border-radius:5px;'>";
            configHtml += "</form>";
        }

        // 設定更新フォームの後にリセットボタンを配置
        configHtml += "<hr>";
        configHtml += "<h3>メンテナンス</h3>";
        configHtml += "<button style='background:#ff4444; color:white; padding:10px; border:none; border-radius:5px;' "
                      "onclick=\"if(confirm('設定を初期状態に戻しますか？')){location.href='/reset_config';}\">"
                      "設定を初期化する</button>";
    }
   
    String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'></head><body>"
                  "<h2>" + String(ssid) + " File Server / CAN Transmission</h2>"
                  "<div style='background:#f0f0f0; padding:10px; border-radius:5px; margin-bottom:10px;'>" + fileStatus + "</div>"
                  "<form method='POST' action='/upload' enctype='multipart/form-data'>"
                  "<input type='file' name='upload'><br><br>"
                  "<input type='submit' value='アップロード' style='width:100px; height:30px;'>"
                  "</form>"
                  "<hr>"
                  "<button style='background:#e1ff00; width:200px; height:50px;' onclick=\"fetch('/process').then(()=>alert('処理を開始しました（シリアルを確認してください）'))\">ファイルを16byteずつ処理</button>";
    html += "<hr>" + configHtml;
    html += "</body></html>";
    
    server.send(200, "text/html", html);
}

// ファイルアップロード処理（進捗表示付き）
void handleFileUpload() {
    HTTPUpload& upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        isUploading = true; // 受信表示停止
        lastPercent = 0;
        // ヘッダーからファイルサイズを取得（文字列を整数に変換）
        total_file_size = server.header("Content-Length").toInt();
        
        if (LittleFS.exists(filename)) LittleFS.remove(filename);
        
        Serial.printf("Upload Start: %s (Total: %d bytes)\n", upload.filename.c_str(), total_file_size);
        #ifdef USE_LCD
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.printf("Uploading...\n%s", upload.filename.c_str());
        #endif
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        File f = LittleFS.open(filename, "a");
        if (f) {
            f.write(upload.buf, upload.currentSize);
            f.close();
        }

        // 進捗計算
        if (total_file_size > 0) {
            size_t progress = (upload.totalSize * 100) / total_file_size;
            if (progress != lastPercent) {
                lastPercent = progress;
                Serial.printf("Progress: %d%%\n", progress);
                #ifdef USE_LCD
                M5.Lcd.fillRect(0, 40, 160, 40, BLACK); // 表示エリアをクリア
                M5.Lcd.setCursor(0, 40);
                M5.Lcd.printf("Progress: %d%%", progress);
                M5.Lcd.fillRect(0, 60, (progress * 160 / 100), 10, GREEN); // プログレスバー
                #endif
            }
        }

    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Upload Finished: %u bytes\n", upload.totalSize);
        #ifdef USE_LCD
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.println("Upload Done!");
        M5.Lcd.printf("Final Size: %u", upload.totalSize);
        #endif
        
        delay(1000);
        isUploading = false; // 受信表示再開
        server.sendHeader("Location", "/");
        server.send(303);
    }
}

// 設定をデフォルトに戻す関数
void resetConfig() {
    File f = LittleFS.open(config_path, "w");
    if (f) {
        // ここにデフォルトのJSON構造を定義
        f.println("{\"packet_gap\": 100, \"chunk_interval\": 100, \"id1_hex\": \"123\", \"id2_hex\": \"124\"}");
        f.close();
        Serial.println("Config has been reset to default.");
    }
}

// 設定保存処理
void handleSaveConfig() {
    if (LittleFS.exists(config_path)) {
        // 現在のJSONを読み込み
        File f_read = LittleFS.open(config_path, "r");
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, f_read);
        f_read.close();

        if (!error) {
            // server.args() で POST された全パラメータをループ処理
            for (int i = 0; i < server.args(); i++) {
                String key = server.argName(i);
                String val = server.arg(i);

                // ArduinoJson V7: キーが存在するか(nullでないか)を確認
                if (!doc[key].isNull()) {
                    // 元の型に合わせて保存（数値型への自動変換）
                    if (doc[key].is<int>()) {
                        doc[key] = val.toInt();
                    } else if (doc[key].is<float>()) {
                        doc[key] = val.toFloat();
                    } else {
                        doc[key] = val; // 文字列として保存
                    }
                }
            }

            // ファイルに保存
            File f_write = LittleFS.open(config_path, "w");
            serializeJson(doc, f_write);
            f_write.close();
            
            Serial.println("Config Updated via Web");
        }
    }

    // ルートに戻す
    server.sendHeader("Location", "/");
    server.send(303);
}

// JSONからグローバル変数に設定を反映する関数
void updateParamsFromJson() {
    if (!LittleFS.exists(config_path)) {
        resetConfig();
        return;
    }

    File f = LittleFS.open(config_path, "r");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, f);
    f.close();

    if (error) {
        Serial.println("Failed to read config, resetting...");
        resetConfig(); // パース失敗時は初期化
        return;
    }

    // JSONの値をグローバル変数に反映 (V7推奨の is<T>() による存在確認)
    if (doc["packet_gap"].is<uint32_t>()) {
        CAN_PACKET_GAP = doc["packet_gap"];
    }
    
    if (doc["chunk_interval"].is<uint32_t>()) {
        CAN_CHUNK_INTERVAL = doc["chunk_interval"];
    }
    
    // IDはWebから16進数文字列で入力されることを想定
    if (doc["id1_hex"].is<const char*>()) {
        CAN_SEND_ID1 = strtoul(doc["id1_hex"], NULL, 16);
    }
    
    if (doc["id2_hex"].is<const char*>()) {
        CAN_SEND_ID2 = strtoul(doc["id2_hex"], NULL, 16);
    }

    Serial.printf("Params updated: GAP=%u, Interval=%u, ID1=0x%X, ID2=0x%X\n", 
                  CAN_PACKET_GAP, CAN_CHUNK_INTERVAL, CAN_SEND_ID1, CAN_SEND_ID2);
}

// Webエンドポイント用ハンドラ
void handleResetConfig() {
    resetConfig();
    server.sendHeader("Location", "/");
    server.send(303);
}

// 分割処理用
void processFile() {
    // 送信前に最新の設定を読み込む
    updateParamsFromJson();

    File f = LittleFS.open(filename, "r");
    if (!f) {
        #ifndef USE_LCD
            // ファイルがない場合は警告として一瞬黄色に
            M5.dis.drawpix(0, 0xffff00);
            delay(500);
            M5.dis.drawpix(0, 0x000000);
        #endif
        return;
    }

    Serial.println("--- CAN Transmission Start ---");
    Serial.printf("Settings: ID1=0x%X, ID2=0x%X, Gap=%d, Interval=%d\n", 
                  CAN_SEND_ID1, CAN_SEND_ID2, CAN_PACKET_GAP, CAN_CHUNK_INTERVAL);
    uint8_t buffer[CHUNK_SIZE];
    while (f.available()) {
        int bytesRead = f.read(buffer, CHUNK_SIZE);
        // --- デバッグ用シリアル出力 ---
        Serial.printf("[%04X]: ", f.position() - bytesRead);
        for (int i = 0; i < bytesRead; i++) Serial.printf("%02X ", buffer[i]);
        Serial.println();

        // 画面あるいはLEDで送信表示
        #ifdef USE_LCD
            // 進捗表示（LCD機種のみ）
            M5.Lcd.fillRect(0, 40, 160, 40, BLACK); // 表示エリアをクリア
            M5.Lcd.setCursor(0, 40);
            // 全体 byte 数と現在の byte 位置を表示
            M5.Lcd.printf("Processing: %u / %u bytes", f.position(), f.size());
        #else
            // LED点灯（Atom LiteはLEDを一瞬青に）
            // 間隔を設けられないので、時々消灯を入れることにより点滅を演出
            if (f.position() % (CHUNK_SIZE * 10) == 0) { // 10回ごとに消灯
                M5.dis.drawpix(0, 0x000000); // 消灯
            } else {
                M5.dis.drawpix(0, 0x0000ff); // 青点灯
            }
        #endif

        // CANは最大8byteなので16byteを2回に分けて送信
        if (bytesRead > 0) {
            // 1パケット目 (最大8byte) 送信
            uint8_t firstLen = (bytesRead > 8) ? 8 : bytesRead;
            sendCAN(CAN_SEND_ID1, buffer, firstLen);
            // 8byteを超えるデータがある場合、2パケット目を送信
            if (bytesRead > 8) {
                delay(CAN_PACKET_GAP); // パケット間の衝突防止
                sendCAN(CAN_SEND_ID2, buffer + 8, bytesRead - 8);
            }
        }
        delay(CAN_CHUNK_INTERVAL); // 次の16byteセットまでの間隔
    }
    f.close();
    #ifndef USE_LCD
        M5.dis.drawpix(0, 0x000000); // 終了後に完全に消灯
    #endif
    Serial.println("--- CAN Transmission End ---");
}

void setup() {
    // 機種ごとの初期化
    #ifdef USE_LCD
        M5.begin();
        M5.Lcd.setRotation(3);
    #else
        M5.begin(true, false, true); // Serial, I2C, LED
    #endif

    LittleFS.begin(true);
    // 起動時に設定を読み込む
    updateParamsFromJson();

    setupCAN();
    WiFi.softAP(ssid, password);

    // Content-Lengthヘッダーを取得可能にする
    const char * headerkeys[] = {"Content-Length"} ;
    server.collectHeaders(headerkeys, 1);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/upload", HTTP_POST, []() { server.send(200); }, handleFileUpload);
    server.on("/process", HTTP_GET, []() {
        processFile();
        server.send(200, "text/plain", "OK");
    });
    server.on("/save_config", HTTP_POST, handleSaveConfig);
    server.on("/reset_config", HTTP_GET, handleResetConfig);

    server.begin();
    #ifdef USE_LCD
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("AP Mode OK");
    M5.Lcd.println(WiFi.softAPIP().toString());
    M5.Lcd.println("Ready: CAN Monitor");
    #endif
}

// --- 受信モニター用設定 ---
bool DEBUG_DUMMY_CAN = false; // trueにするとランダムで受信マークが出る
const uint32_t SCAN_INTERVAL_MS = 100; // 1コマの時間
const int MONITOR_ID_COUNT = 3; 
const uint32_t MONITOR_IDS[MONITOR_ID_COUNT] = {0x123, 0x124, 0x100};
bool idReceivedFlags[MONITOR_ID_COUNT] = {false};

int scanX = 0;           
uint32_t lastScanTime = 0;
int heartBeatStep = 0;

void loop() {
    server.handleClient();

    if (!isUploading) {
        // 1. CAN受信チェック
        twai_message_t rx_msg;
        if (twai_receive(&rx_msg, 0) == ESP_OK) {
            // シリアル出力（詳細はシリアルで確認）
            // CAN ID と受信したデータを16進数で表示
            Serial.printf("RX ID: 0x%03X", rx_msg.identifier);
            Serial.print(", Data: ");
            for (int i = 0; i < rx_msg.data_length_code; i++) {
                Serial.printf("%02X ", rx_msg.data[i]);
            }
            Serial.println();
            // 受信したIDがモニター対象のIDリストにあるかチェックしてフラグを立てる
            for (int i = 0; i < MONITOR_ID_COUNT; i++) {
                if (rx_msg.identifier == MONITOR_IDS[i]) idReceivedFlags[i] = true;
            }
            #ifndef USE_LCD
                M5.dis.drawpix(0, 0x00ff00); // Atom Liteなら受信時にLEDを一瞬緑に
            #endif
        }

        // 2. デバッグ用ダミーデータ生成 (0.2%の確率で受信を偽装)
        if (DEBUG_DUMMY_CAN) {
            if (random(1000) < 2) { // 確率0.2%
                idReceivedFlags[random(MONITOR_ID_COUNT)] = true;
            }
        }

        // 3. 描画更新処理 (LCDがある機種のみ)
        #ifdef USE_LCD
        if (millis() - lastScanTime >= SCAN_INTERVAL_MS) {
            lastScanTime = millis();

            // --- 生存確認インジケータ (右下の隅 y=70付近に配置) ---
            const char* hb_chars = "|/-\\";
            M5.Lcd.setTextColor(DARKGREY, BLACK); // 文字は標準的なグレー
            M5.Lcd.setCursor(152, 72);
            M5.Lcd.printf("%c", hb_chars[heartBeatStep % 4]);
            heartBeatStep++;

            // --- チャート描画レイアウト ---
            int chartStartX = 40; // IDラベル用の余白
            int currentX = chartStartX + scanX;
            int eraserX = chartStartX + ((scanX + 1) % (160 - chartStartX));

            // チャートエリア(y=40-80)のみを消去
            M5.Lcd.fillRect(eraserX, 40, 2, 40, BLACK); 

            for (int i = 0; i < MONITOR_ID_COUNT; i++) {
                int yPos = 42 + (i * 12); // 下半分に収まるよう調整
                
                // 左端(scanX=0)でIDラベルを描画
                if (scanX == 0) {
                    M5.Lcd.setTextColor(WHITE, BLACK); // ID文字は白でハッキリ
                    M5.Lcd.setCursor(2, yPos);
                    M5.Lcd.printf("%03X:", MONITOR_IDS[i]);
                }

                // 受信状況の描画
                // 受信あり：シアン(CYAN) - 明るく見やすい
                // 受信なし：中間グレー(0x7BEF) - 背景よりは明るい
                uint16_t color = idReceivedFlags[i] ? CYAN : 0x7BEF; 
                
                // 受信時は少し太めの線(2px幅)にして強調
                if (idReceivedFlags[i]) {
                    M5.Lcd.drawFastVLine(currentX, yPos, 10, color);
                    if (currentX < 159) M5.Lcd.drawFastVLine(currentX + 1, yPos, 10, color);
                } else {
                    M5.Lcd.drawPixel(currentX, yPos + 5, color); // 未受信時は点(1px)のみ
                }
                
                idReceivedFlags[i] = false; 
            }

            // 描画位置を進める
            scanX++;
            if (scanX >= (160 - chartStartX)) scanX = 0; 
        }
        #else
        // LCDがない場合（Atom等）の処理、フラグだけ定期的にクリア
        if (millis() - lastScanTime >= SCAN_INTERVAL_MS) {
            lastScanTime = millis();
            M5.dis.drawpix(0, 0x000000); // LED消灯
            for (int i = 0; i < MONITOR_ID_COUNT; i++) idReceivedFlags[i] = false;
        }
        #endif
    }
}
