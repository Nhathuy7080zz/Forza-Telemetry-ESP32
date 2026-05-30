#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#pragma comment(lib, "ws2_32.lib")

#define COM_PORT "\\\\.\\COM6"
#define BAUD_RATE 115200
#define UDP_PORT 5607
#define FLYDIGI_PORT 5300

inline int f_to_c(float f_temp) {
    return (int)((f_temp - 32.0f) * 5.0f / 9.0f);
}

int main() {
    HANDLE hSerial = CreateFileA(COM_PORT, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (hSerial == INVALID_HANDLE_VALUE) {
        printf("Loi mo cong %s\n", COM_PORT);
        return 1;
    }


    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);
    dcbSerialParams.BaudRate = BAUD_RATE;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    SetCommState(hSerial, &dcbSerialParams);
    
    Sleep(2000);

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in serverAddr = { 0 };
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(UDP_PORT);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(sock, (sockaddr*)&serverAddr, sizeof(serverAddr));

    sockaddr_in flydigiAddr = { 0 };
    flydigiAddr.sin_family = AF_INET;
    flydigiAddr.sin_port = htons(FLYDIGI_PORT);
    flydigiAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    printf("Server C++ dang chay.\nNghe Forza UDP %d -> Flydigi %d & ESP32 %s\n", UDP_PORT, FLYDIGI_PORT, COM_PORT);

    char buffer[1024];
    char json[1024];

    while (1) {
        int bytes = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
        
        if (bytes > 0) {
            // Forward ngay lập tức cho Flydigi để Adaptive Trigger hoạt động
            sendto(sock, buffer, bytes, 0, (sockaddr*)&flydigiAddr, sizeof(flydigiAddr));
            
            // Forza packet có thể là 311 hoặc 324 hoặc 331 bytes
            if (bytes >= 311) {
                // Bỏ qua check is_race_on để test xem dữ liệu có truyền không. 
                // Có thể FH6 luôn thiết lập IsRaceOn nằm ở vị trí khác.
                float rpm       = *(float*)(buffer + 16);
                float glat      = *(float*)(buffer + 20) / 9.81f;
                float glon      = *(float*)(buffer + 28) / 9.81f;
                float susp_fl   = *(float*)(buffer + 68);
                float susp_fr   = *(float*)(buffer + 72);
                float susp_rl   = *(float*)(buffer + 76);
                float susp_rr   = *(float*)(buffer + 80);
                float slip_fl   = *(float*)(buffer + 84);
                float slip_fr   = *(float*)(buffer + 88);
                float slip_rl   = *(float*)(buffer + 92);
                float slip_rr   = *(float*)(buffer + 96);
                float speed_ms  = *(float*)(buffer + 256);
                float power_w   = *(float*)(buffer + 260);
                float torque_nm = *(float*)(buffer + 264);
                float temp_fl   = *(float*)(buffer + 268);
                float temp_fr   = *(float*)(buffer + 272);
                float temp_rl   = *(float*)(buffer + 276);
                float temp_rr   = *(float*)(buffer + 280);
                float boost_psi = *(float*)(buffer + 284);

                uint8_t accel  = buffer[315];
                uint8_t brake  = buffer[316];
                uint8_t clutch = buffer[317];
                uint8_t ebrake = buffer[318];
                uint8_t gear   = buffer[319];
                int8_t steer   = (int8_t)buffer[320];

                char gear_str[4];
                if (gear == 0) strcpy(gear_str, "\"R\"");
                else if (gear == 11) strcpy(gear_str, "\"N\"");
                else snprintf(gear_str, sizeof(gear_str), "\"%d\"", gear);

                int len = snprintf(json, sizeof(json),
                    "{\"speed\":%d,\"rpm\":%d,\"hp\":%d,\"torque\":%d,\"boost\":%.2f,"
                    "\"gear\":%s,\"accel\":%d,\"brake\":%d,\"clutch\":%d,\"ebrake\":%d,\"steer\":%d,"
                    "\"glat\":%.2f,\"glon\":%.2f,"
                    "\"susp\":{\"fl\":%.2f,\"fr\":%.2f,\"rl\":%.2f,\"rr\":%.2f},"
                    "\"tires\":{"
                    "\"fl\":{\"t\":%d,\"s\":%.2f},\"fr\":{\"t\":%d,\"s\":%.2f},"
                    "\"rl\":{\"t\":%d,\"s\":%.2f},\"rr\":{\"t\":%d,\"s\":%.2f}"
                    "}}\n",
                    (int)(speed_ms * 3.6f), (int)rpm, (int)(power_w * 0.00134102f), (int)torque_nm, (boost_psi - 14.7f) * 0.0689476f,
                    gear_str, (int)(accel / 255.0f * 100.0f), (int)(brake / 255.0f * 100.0f), (int)(clutch / 255.0f * 100.0f), (int)(ebrake / 255.0f * 100.0f), (int)(steer / 127.0f * 100.0f),
                    glat, glon,
                    susp_fl, susp_fr, susp_rl, susp_rr,
                    f_to_c(temp_fl), fabs(slip_fl), f_to_c(temp_fr), fabs(slip_fr),
                    f_to_c(temp_rl), fabs(slip_rl), f_to_c(temp_rr), fabs(slip_rr)
                );

                DWORD bytesWritten;
                WriteFile(hSerial, json, len, &bytesWritten, NULL);
            }
        }
    }
    
    CloseHandle(hSerial);
    WSACleanup();
    return 0;
}