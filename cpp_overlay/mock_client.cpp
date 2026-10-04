// Тестовый клиент: имитирует мод Minecraft и шлёт координаты в оверлей (без запуска игры).
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

#pragma pack(push, 1)
struct PlayerCoords {
    double x, y, z;
    float yaw, pitch;
    int32_t hasTarget;
    int32_t aiming;
    double targetX, targetY, targetZ;
    float aimYaw, aimPitch;
};
#pragma pack(pop)
static_assert(sizeof(PlayerCoords) == 72, "packet must be 72 bytes");

int main() {
    printf("[Mock Client] Connecting to \\\\.\\pipe\\mc_coords...\n");
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    while (hPipe == INVALID_HANDLE_VALUE) {
        hPipe = CreateFileW(L"\\\\.\\pipe\\mc_coords", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) { printf("Waiting for MCOverlay.exe...\n"); Sleep(1000); }
    }
    printf("[Mock Client] Connected. Sending fake coordinates (Ctrl+C to stop).\n");

    double t = 0;
    for (;;) {
        PlayerCoords c;
        c.x = 120.0 + cos(t) * 40.0;
        c.y = 64.0 + sin(t * 0.5) * 3.0;
        c.z = -85.0 + sin(t) * 40.0;
        c.yaw = (float)fmod(t * 40.0, 360.0) - 180.0f;
        c.pitch = (float)(sin(t * 0.7) * 30.0);
        c.hasTarget = 1;
        c.aiming = ((int)(t * 20.0) % 4) != 0;
        c.targetX = c.x + 8.0;
        c.targetY = c.y;
        c.targetZ = c.z + 6.0;
        c.aimYaw = c.yaw + 12.0f;
        c.aimPitch = c.pitch - 3.0f;
        DWORD written = 0;
        if (!WriteFile(hPipe, &c, sizeof(c), &written, NULL)) break;
        t += 0.05;
        Sleep(50);
    }
    CloseHandle(hPipe);
    return 0;
}
