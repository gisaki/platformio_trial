#include <M5StickC.h> // M5StickCPlusの場合は M5StickCPlus.h

void setup() {
    // M5StickCの初期化
    M5.begin();
    
    // 画面
    M5.Lcd.begin();
    M5.Lcd.setRotation(3);
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE, BLACK);

    // テキスト
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("Hello M5StickC");
    M5.Lcd.print("PlatformIO");
}

void loop() {
    // 必要に応じてループ処理
}
