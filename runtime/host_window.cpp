#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "host_window.h"

namespace {
std::atomic<uint8_t*> gGuestBase{nullptr};
std::atomic<uint32_t> gFrontBuffer{0};
std::atomic<uint32_t> gFrontWidth{1280};
std::atomic<uint32_t> gFrontHeight{720};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const uint32_t address = gFrontBuffer.load();
        uint8_t* base = gGuestBase.load();
        const uint32_t width = gFrontWidth.load();
        const uint32_t height = gFrontHeight.load();
        if (base && address && width && height) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(width);
            info.bmiHeader.biHeight = -static_cast<LONG>(height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, width, height,
                base + address, &info, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

void StartHostWindow(uint8_t* guestBase) {
    gGuestBase.store(guestBase);
    std::thread([] {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"RRXXNativeWindow";
        RegisterClassW(&windowClass);
        RECT rect{0, 0, 1280, 720};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        HWND window = CreateWindowExW(0, windowClass.lpszClassName,
            L"Rumble Roses XX - XenonRecomp", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance, nullptr);
        if (!window) return;
        ShowWindow(window, SW_SHOWDEFAULT);
        MSG message{};
        while (true) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) return;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            InvalidateRect(window, nullptr, FALSE);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }).detach();
}

void UpdateHostFrontBuffer(uint32_t guestAddress, uint32_t width, uint32_t height) {
    gFrontBuffer.store(guestAddress);
    gFrontWidth.store(std::clamp(width, 1u, 4096u));
    gFrontHeight.store(std::clamp(height, 1u, 4096u));
}
