#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "render/Renderer.h"
#include "core/Game.h"
#include "core/JobSystem.h"
#include <chrono>
#include <iostream>

namespace {
    Legionfall::Renderer* g_renderer = nullptr;
    Legionfall::Game* g_game = nullptr;
    Legionfall::JobSystem* g_jobSystem = nullptr;
    Legionfall::InputState g_input{};
    bool g_running = true;
    bool g_minimized = false;
    uint32_t g_width = 1280, g_height = 720;
    const wchar_t* CLASS_NAME = L"LegionfallWindow";

    // Phase 2: Start with 1000 enemies!
    constexpr uint32_t INITIAL_ENEMIES = 1000;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE: g_running = false; return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_SIZE: {
            uint32_t w = LOWORD(lParam), h = HIWORD(lParam);
            g_minimized = (w == 0 || h == 0);
            if (!g_minimized) { g_width = w; g_height = h; if (g_renderer) g_renderer->onResize(w, h); }
            return 0;
        }
        case WM_KEYDOWN:
            switch (wParam) {
                case 'W': case VK_UP: g_input.moveUp = true; break;
                case 'S': case VK_DOWN: g_input.moveDown = true; break;
                case 'A': case VK_LEFT: g_input.moveLeft = true; break;
                case 'D': case VK_RIGHT: g_input.moveRight = true; break;
                case VK_SPACE: g_input.attack = true; break;
                case 'P': g_input.toggleParallel = true; break;
                case 'H': g_input.toggleHeavyWork = true; break;
                case VK_ESCAPE: g_running = false; break;
            }
            return 0;
        case WM_KEYUP:
            switch (wParam) {
                case 'W': case VK_UP: g_input.moveUp = false; break;
                case 'S': case VK_DOWN: g_input.moveDown = false; break;
                case 'A': case VK_LEFT: g_input.moveLeft = false; break;
                case 'D': case VK_RIGHT: g_input.moveRight = false; break;
                case VK_SPACE: g_input.attack = false; break;
                case 'P': g_input.toggleParallel = false; break;
                case 'H': g_input.toggleHeavyWork = false; break;
            }
            return 0;
        default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    AllocConsole();
    FILE* fp; freopen_s(&fp, "CONOUT$", "w", stdout); freopen_s(&fp, "CONOUT$", "w", stderr);
    
    std::cout << "========================================" << std::endl;
    std::cout << "  Legionfall - Phase 2: Instanced Swarm " << std::endl;
    std::cout << "========================================" << std::endl;

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc; wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    RECT r = {0, 0, (LONG)g_width, (LONG)g_height}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Legionfall - Phase 2", WS_OVERLAPPEDWINDOW,
        (sw - (r.right - r.left)) / 2, (sh - (r.bottom - r.top)) / 2,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInstance, nullptr);

    g_jobSystem = new Legionfall::JobSystem();
    g_game = new Legionfall::Game();
    g_renderer = new Legionfall::Renderer();
    
    std::cout << "[OK] JobSystem: " << g_jobSystem->threadCount() << " threads" << std::endl;

    if (!g_renderer->init(hwnd, hInstance, g_width, g_height)) {
        MessageBoxW(hwnd, L"Vulkan init failed!", L"Error", MB_OK);
        return 1;
    }

    g_game->init(INITIAL_ENEMIES);
    std::cout << "[OK] Game initialized with " << INITIAL_ENEMIES << " enemies" << std::endl;
    
    ShowWindow(hwnd, nCmdShow);
    SetForegroundWindow(hwnd);
    
    std::cout << "========================================" << std::endl;
    std::cout << " Controls: WASD=Move, ESC=Exit          " << std::endl;
    std::cout << "========================================" << std::endl;

    auto lastTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    double fpsTimer = 0.0;
    
    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!g_running) break;

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;
        if (g_minimized) { Sleep(10); continue; }

        g_game->update(dt, g_input, g_jobSystem);
        
        // Camera follows hero (optional - remove for static camera)
        float hx, hy;
        g_game->getHeroPosition(hx, hy);
        // g_renderer->setCameraPosition(hx, hy);  // Uncomment to follow hero
        
        g_renderer->updateInstanceBuffer(g_game->getInstanceData());
        g_renderer->drawFrame();

        frameCount++;
        fpsTimer += dt;
        if (fpsTimer >= 1.0) {
            auto& stats = g_game->getStats();
            std::cout << "FPS: " << frameCount << " | Instances: " << stats.aliveCount + 1 << std::endl;
            frameCount = 0;
            fpsTimer = 0.0;
        }
    }

    std::cout << "Shutting down..." << std::endl;
    delete g_renderer; delete g_game; delete g_jobSystem;
    DestroyWindow(hwnd);
    Sleep(300);
    FreeConsole();
    return 0;
}