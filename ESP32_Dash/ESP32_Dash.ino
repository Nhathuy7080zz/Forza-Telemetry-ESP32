#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "index_html.h" // Chứa chuỗi index_html PROGMEM

// Thông tin WiFi
const char* ssid = "Homy Home";
const char* password = "homy@linhtrung";

// Cấu hình màn hình OLED I2C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define I2C_SDA 4
#define I2C_SCL 5

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

#pragma pack(push, 1)
typedef struct {
    uint8_t magic;      // 0xAA
    uint8_t gear;       // 0=R, 1-10
    float speed;        // km/h
    float hp;
    float torque;
    float boost;        // bar
    float glat;
    float glon;
    float rpm;
    int8_t steer;       // -127 to 127
    uint8_t accel;      // 0-100
    uint8_t brake;      // 0-100
    uint8_t clutch;     // 0-100
    uint8_t ebrake;     // 0-100
    float susp[4];      // FL, FR, RL, RR
    float tire_t[4];    // Temp
    float tire_s[4];    // Slip
    uint8_t checksum;
} SerialPayload;
#pragma pack(pop)

SerialPayload payload;
String telemetryJson = "{}";
unsigned long lastOledUpdate = 0;
unsigned long lastSerialRecv = 0;

uint8_t calcChecksum(uint8_t* data, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum ^= data[i];
    return sum;
}

void setupOLED() {
    Wire.begin(I2C_SDA, I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
    } else {
        display.setRotation(2); // rotate display 180 degrees
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(0, 10);
        display.println("Connecting WiFi...");
        display.println(ssid);
        display.display();
    }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("WS Client connected: %u\n", client->id());
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("WS Client disconnected: %u\n", client->id());
    }
}

void setup() {
    Serial.begin(115200);
    
    setupOLED();

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi Connected!");
    display.println(WiFi.localIP());
    display.display();

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.begin();
}

void updateOLED() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0, 0);
    
    // Tốc độ (km/h)
    display.print((int)payload.speed);
    display.setTextSize(1);
    display.print(" kmh");
    
    // Số
    display.setCursor(90, 0);
    display.setTextSize(2);
    if (payload.gear == 0) display.print("R");
    else display.print((int)payload.gear);

    // Vòng tua (rpm)
    display.setTextSize(2);
    display.setCursor(0, 25);
    display.print((int)payload.rpm);
    display.setTextSize(1);
    display.print(" rpm");

    // Thanh vòng tua ảo
    int barWidth = map((int)payload.rpm, 0, 10000, 0, 128);
    display.drawRect(0, 48, 128, 10, SSD1306_WHITE);
    display.fillRect(0, 48, barWidth, 10, SSD1306_WHITE);

    display.display();
}

void loop() {
    ws.cleanupClients();

    if (Serial.available() >= sizeof(SerialPayload)) {
        if (Serial.read() == 0xAA) {
            uint8_t buffer[sizeof(SerialPayload)];
            buffer[0] = 0xAA;
            Serial.readBytes(buffer + 1, sizeof(SerialPayload) - 1);
            
            uint8_t chk = calcChecksum(buffer, sizeof(SerialPayload) - 1);
            if (chk == buffer[sizeof(SerialPayload) - 1]) {
                memcpy(&payload, buffer, sizeof(SerialPayload));
                
                // Parse payload -> JSON manually to be fast
                char json[512];
                snprintf(json, sizeof(json), 
                    "{\"gear\":%d,\"speed\":%.1f,\"hp\":%.1f,\"torque\":%.1f,\"boost\":%.2f,"
                    "\"glat\":%.2f,\"glon\":%.2f,\"rpm\":%.1f,\"steer\":%d,\"accel\":%d,"
                    "\"brake\":%d,\"clutch\":%d,\"ebrake\":%d,"
                    "\"susp\":{\"fl\":%.2f,\"fr\":%.2f,\"rl\":%.2f,\"rr\":%.2f},"
                    "\"tires\":{\"fl\":{\"t\":%.0f,\"s\":%.2f},\"fr\":{\"t\":%.0f,\"s\":%.2f},"
                    "\"rl\":{\"t\":%.0f,\"s\":%.2f},\"rr\":{\"t\":%.0f,\"s\":%.2f}}}",
                    payload.gear, payload.speed, payload.hp, payload.torque, payload.boost,
                    payload.glat, payload.glon, payload.rpm, payload.steer, payload.accel,
                    payload.brake, payload.clutch, payload.ebrake,
                    payload.susp[0], payload.susp[1], payload.susp[2], payload.susp[3],
                    payload.tire_t[0], payload.tire_s[0], payload.tire_t[1], payload.tire_s[1],
                    payload.tire_t[2], payload.tire_s[2], payload.tire_t[3], payload.tire_s[3]
                );
                
                telemetryJson = String(json);
                lastSerialRecv = millis();

                // Broadcast
                ws.textAll(telemetryJson);
            }
        }
    }

    if (millis() - lastOledUpdate > 100) { // Cập nhật màn hình ở 10fps
        if (millis() - lastSerialRecv < 1000) {
            updateOLED();
        } else {
            // Không có dữ liệu mới từ game, thường là đang paused hoặc ở menu
            display.clearDisplay();
            display.setTextSize(1);
            display.setCursor(0, 0);
            display.println(WiFi.localIP());
            display.setCursor(0, 20);
            display.println("PAUSED");
            display.setCursor(0, 32);
            display.println("No game data");
            display.display();
        }
        lastOledUpdate = millis();
    }
}