#include <M5StickC.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

const char *ssid = "M5StickC-Server";
const char *password = "12345678";
const char *filename = "/uploaded.bin";
const size_t CHUNK_SIZE = 16; 

WebServer server(80);
size_t total_file_size = 0;
size_t lastPercent = 0;

// Web画面の表示
void handleRoot() {
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

    String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'></head><body>"
                  "<h2>M5StickC File Server</h2>"
                  "<div style='background:#f0f0f0; padding:10px; border-radius:5px; margin-bottom:10px;'>" + fileStatus + "</div>"
                  "<form method='POST' action='/upload' enctype='multipart/form-data'>"
                  "<input type='file' name='upload'><br><br>"
                  "<input type='submit' value='アップロード' style='width:100px; height:30px;'>"
                  "</form>"
                  "<hr>"
                  "<button style='background:#e1ff00; width:200px; height:50px;' onclick=\"fetch('/process').then(()=>alert('処理を開始しました（シリアルを確認してください）'))\">ファイルを16byteずつ処理</button>"
                  "</body></html>";
    
    server.send(200, "text/html", html);
}

// ファイルアップロード処理（進捗表示付き）
void handleFileUpload() {
    HTTPUpload& upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        lastPercent = 0;
        // ヘッダーからファイルサイズを取得（文字列を整数に変換）
        total_file_size = server.header("Content-Length").toInt();
        
        if (LittleFS.exists(filename)) LittleFS.remove(filename);
        
        Serial.printf("Upload Start: %s (Total: %d bytes)\n", upload.filename.c_str(), total_file_size);
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.printf("Uploading...\n%s", upload.filename.c_str());
        
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
                
                M5.Lcd.fillRect(0, 40, 160, 40, BLACK); // 表示エリアをクリア
                M5.Lcd.setCursor(0, 40);
                M5.Lcd.printf("Progress: %d%%", progress);
                M5.Lcd.fillRect(0, 60, (progress * 160 / 100), 10, GREEN); // プログレスバー
            }
        }

    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Upload Finished: %u bytes\n", upload.totalSize);
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.println("Upload Done!");
        M5.Lcd.printf("Final Size: %u", upload.totalSize);
        
        delay(1000);
        server.sendHeader("Location", "/");
        server.send(303);
    }
}

// 分割処理用
void processFile() {
    File f = LittleFS.open(filename, "r");
    if (!f) return;
    Serial.println("--- Start ---");
    uint8_t buffer[CHUNK_SIZE];
    while (f.available()) {
        int bytesRead = f.read(buffer, CHUNK_SIZE);
        Serial.printf("[%04X]: ", f.position() - bytesRead);
        for (int i = 0; i < bytesRead; i++) Serial.printf("%02X ", buffer[i]);
        Serial.println();
    }
    f.close();
    Serial.println("--- End ---");
}

void setup() {
    M5.begin();
    M5.Lcd.setRotation(3);
    LittleFS.begin(true);

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

    server.begin();
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("AP Mode OK");
    M5.Lcd.println(WiFi.softAPIP().toString());
}

void loop() {
    server.handleClient();
}
