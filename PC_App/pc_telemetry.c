#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <math.h>

#pragma comment(lib, "ws2_32.lib")

#pragma pack(push, 1)
// The payload sent over serial to ESP32 (Compact to ensure 115200 baud speed)
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

// Standard Forza Horizon Data Out Packet structure (Data Out V2)
typedef struct {
    int32_t isRaceOn; 
    uint32_t timestampMS; 
    float engineMaxRpm; 
    float engineIdleRpm; 
    float currentEngineRpm; 
    float accX; float accY; float accZ;
    float velX; float velY; float velZ;
    float angVelX; float angVelY; float angVelZ;
    float yaw; float pitch; float roll; 
    float normSuspTravelFL; float normSuspTravelFR; float normSuspTravelRL; float normSuspTravelRR; 
    float tireSlipRatioFL; float tireSlipRatioFR; float tireSlipRatioRL; float tireSlipRatioRR; 
    float wheelRotSpeedFL; float wheelRotSpeedFR; float wheelRotSpeedRL; float wheelRotSpeedRR; 
    int32_t wheelOnRumbleFL; int32_t wheelOnRumbleFR; int32_t wheelOnRumbleRL; int32_t wheelOnRumbleRR; 
    float wheelInPuddleFL; float wheelInPuddleFR; float wheelInPuddleRL; float wheelInPuddleRR; 
    float surfRumbleFL; float surfRumbleFR; float surfRumbleRL; float surfRumbleRR; 
    float tireSlipAngleFL; float tireSlipAngleFR; float tireSlipAngleRL; float tireSlipAngleRR; 
    float tireCombinedSlipFL; float tireCombinedSlipFR; float tireCombinedSlipRL; float tireCombinedSlipRR; 
    float suspTravelMetersFL; float suspTravelMetersFR; float suspTravelMetersRL; float suspTravelMetersRR; 
    int32_t carOrdinal; 
    int32_t carClass; 
    int32_t carPerformanceIndex;
    int32_t drivetrainType;
    int32_t numCylinders; 
    
    // V2 Dash additions
    float posX; float posY; float posZ;
    float speed; 
    float power; 
    float torque; 
    float tireTempFL; float tireTempFR; float tireTempRL; float tireTempRR; 
    float boost; 
    float fuel; 
    float distanceTraveled; 
    float bestLap; 
    float lastLap; 
    float currentLap; 
    float currentRaceTime; 
    uint16_t lapNumber; 
    uint8_t racePosition; 
    uint8_t accel;  
    uint8_t brake;  
    uint8_t clutch; 
    uint8_t handBrake; 
    uint8_t gear; 
    int8_t steer; 
    int8_t normDrivingLine; 
    int8_t normAIBrakeDiff; 
} ForzaTelemetry;
#pragma pack(pop)

HANDLE hSerial = INVALID_HANDLE_VALUE;

int initSerial(const char* portName) {
    char portPath[32];
    snprintf(portPath, sizeof(portPath), "\\\\.\\%s", portName);

    hSerial = CreateFileA(portPath, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (hSerial == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hSerial, &dcbSerialParams)) return 0;
    
    // ESP32 CH340 usually supports 115200 natively and easily without errors
    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    if (!SetCommState(hSerial, &dcbSerialParams)) return 0;

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    if (!SetCommTimeouts(hSerial, &timeouts)) return 0;

    return 1;
}

uint8_t calcChecksum(uint8_t* data, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum ^= data[i];
    return sum;
}

int main(int argc, char* argv[]) {
    // Config: Allow passing COM port and listen port via arguments
    const char* comPort = "COM6";
    int listenPort = 5301;  // Trò chơi FH6 sẽ gửi data tới port này
    int flydigiPort = 5300; // Forward cho Flydigi trên máy
    
    if (argc > 1) comPort = argv[1];
    if (argc > 2) listenPort = atoi(argv[2]);

    printf("Forza Telemetry Forwarder (C)\n");
    printf("Listening FH6 on UDP %d\n", listenPort);
    printf("Forwarding to Flydigi on UDP %d\n", flydigiPort);
    printf("Sending to ESP32 on %s at 115200 baud\n", comPort);
    
    if (initSerial(comPort)) {
        printf(" -> Serial %s opened successfully!\n", comPort);
    } else {
        printf(" -> Could not open %s. Is ESP32 plugged in? (Will still forward to Flydigi)\n", comPort);
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed.\n"); return 1;
    }

    SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) return 1;

    struct sockaddr_in serverAddr, clientAddr, flydigiAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(listenPort);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udpSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("Bind failed on port %d.\n", listenPort); return 1;
    }

    // Setup address to forward to flydigi
    flydigiAddr.sin_family = AF_INET;
    flydigiAddr.sin_port = htons(flydigiPort);
    inet_pton(AF_INET, "127.0.0.1", &flydigiAddr.sin_addr);

    char buffer[1024];
    int clientLen = sizeof(clientAddr);
    DWORD bytesWritten;
    
    printf("\nWaiting for telemetry data...\n");

    while (1) {
        int bytes = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (struct sockaddr*)&clientAddr, &clientLen);
        if (bytes == SOCKET_ERROR) continue;

        // Xử lý gửi thẳng UDP raw về cho Flydigi để không bị delay
        sendto(udpSocket, buffer, bytes, 0, (struct sockaddr*)&flydigiAddr, sizeof(flydigiAddr));

        // Format v2 parse để gửi xuống ESP32
        if (bytes >= sizeof(ForzaTelemetry)) {
            ForzaTelemetry* fh = (ForzaTelemetry*)buffer;
            
            if (fh->isRaceOn == 0) continue; // Không ở trong race/freeroam

            SerialPayload p;
            p.magic = 0xAA;
            p.gear = fh->gear;
            p.speed = fh->speed * 3.6f; // m/s to km/h
            p.hp = fh->power * 0.00134102f; // Watts to HP
            p.torque = fh->torque;
            p.boost = fh->boost / 14.5038f; // psi to bar roughly, or native depends on game
            p.glat = fh->accX / 9.81f; // G-force
            p.glon = fh->accZ / 9.81f;
            p.rpm = fh->currentEngineRpm;
            p.steer = fh->steer; // -127 to 127
            p.accel = (uint8_t)((fh->accel / 255.0f) * 100);
            p.brake = (uint8_t)((fh->brake / 255.0f) * 100);
            p.clutch = (uint8_t)((fh->clutch / 255.0f) * 100);
            p.ebrake = (uint8_t)((fh->handBrake / 255.0f) * 100);
            
            p.susp[0] = fh->normSuspTravelFL; p.susp[1] = fh->normSuspTravelFR;
            p.susp[2] = fh->normSuspTravelRL; p.susp[3] = fh->normSuspTravelRR;
            
            p.tire_t[0] = (fh->tireTempFL - 32) * 5.0f/9.0f; // F to C if it's in F
            p.tire_t[1] = (fh->tireTempFR - 32) * 5.0f/9.0f;
            p.tire_t[2] = (fh->tireTempRL - 32) * 5.0f/9.0f;
            p.tire_t[3] = (fh->tireTempRR - 32) * 5.0f/9.0f;
            
            p.tire_s[0] = fh->tireCombinedSlipFL;
            p.tire_s[1] = fh->tireCombinedSlipFR;
            p.tire_s[2] = fh->tireCombinedSlipRL;
            p.tire_s[3] = fh->tireCombinedSlipRR;
            
            p.checksum = calcChecksum((uint8_t*)&p, sizeof(SerialPayload) - 1);
            
            if (hSerial != INVALID_HANDLE_VALUE) {
                WriteFile(hSerial, &p, sizeof(SerialPayload), &bytesWritten, NULL);
            }
        }
    }

    closesocket(udpSocket);
    WSACleanup();
    if (hSerial != INVALID_HANDLE_VALUE) CloseHandle(hSerial);
    return 0;
}