#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#pragma pack(push, 1)
// The payload sent over serial to ESP32 (Compact to ensure 115200 baud speed)
typedef struct {
    uint8_t magic;      // 0xAA
    uint8_t gear;       // 0=R, 11=N, 1-10=gears
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

static float read_f32(const uint8_t* data, int offset) {
    float value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static int32_t read_i32(const uint8_t* data, int offset) {
    int32_t value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static int8_t read_i8(const uint8_t* data, int offset) {
    int8_t value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static uint8_t clamp_u8(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static int f_to_c(float f_temp) {
    return (int)((f_temp - 32.0f) * 5.0f / 9.0f);
}

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
    int listenPort = 5607;  // FH6 telemetry port from the reference FH6 Telemetry bridge
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

        // Format parse đúng theo FH6 Telemetry reference (server.py / main.cpp)
        if (bytes >= 321) {
            const uint8_t* fh = (const uint8_t*)buffer;
            if (read_i32(fh, 0) == 0) continue; // Not race/active, keep ESP quiet

            const float rpm = read_f32(fh, 16);
            const float ax = read_f32(fh, 20);
            const float az = read_f32(fh, 28);
            const float speed_ms = read_f32(fh, 256);
            const float power_w = read_f32(fh, 260);
            const float torque_nm = read_f32(fh, 264);
            const float temp_fl = read_f32(fh, 268);
            const float temp_fr = read_f32(fh, 272);
            const float temp_rl = read_f32(fh, 276);
            const float temp_rr = read_f32(fh, 280);
            const float boost_psi = read_f32(fh, 284);
            const float susp_fl = read_f32(fh, 68);
            const float susp_fr = read_f32(fh, 72);
            const float susp_rl = read_f32(fh, 76);
            const float susp_rr = read_f32(fh, 80);
            const float slip_fl = read_f32(fh, 84);
            const float slip_fr = read_f32(fh, 88);
            const float slip_rl = read_f32(fh, 92);
            const float slip_rr = read_f32(fh, 96);
            const uint8_t accel = fh[315];
            const uint8_t brake = fh[316];
            const uint8_t clutch = fh[317];
            const uint8_t ebrake = fh[318];
            const uint8_t gear = fh[319];
            const int8_t steer = read_i8(fh, 320);

            SerialPayload p;
            p.magic = 0xAA;
            p.gear = gear;
            p.speed = speed_ms * 3.6f;
            p.hp = power_w * 0.00134102f;
            p.torque = torque_nm;
            p.boost = (boost_psi - 14.7f) * 0.0689476f;
            p.glat = ax / 9.81f;
            p.glon = az / 9.81f;
            p.rpm = rpm;
            p.steer = steer;
            p.accel = clamp_u8((int)(accel / 255.0f * 100.0f));
            p.brake = clamp_u8((int)(brake / 255.0f * 100.0f));
            p.clutch = clamp_u8((int)(clutch / 255.0f * 100.0f));
            p.ebrake = clamp_u8((int)(ebrake / 255.0f * 100.0f));
            p.susp[0] = susp_fl; p.susp[1] = susp_fr; p.susp[2] = susp_rl; p.susp[3] = susp_rr;
            p.tire_t[0] = (float)f_to_c(temp_fl);
            p.tire_t[1] = (float)f_to_c(temp_fr);
            p.tire_t[2] = (float)f_to_c(temp_rl);
            p.tire_t[3] = (float)f_to_c(temp_rr);
            p.tire_s[0] = fabsf(slip_fl);
            p.tire_s[1] = fabsf(slip_fr);
            p.tire_s[2] = fabsf(slip_rl);
            p.tire_s[3] = fabsf(slip_rr);
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