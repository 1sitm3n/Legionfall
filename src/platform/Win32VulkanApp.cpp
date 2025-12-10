#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "render/Renderer.h"
#include "core/Game.h"
#include "core/JobSystem.h"
#include <chrono>
#include <iostream>
#include <sstream>

namespace {
    Legionfall::Renderer* g_renderer = nullptr;
    Legionfall::Game* g_game = nullptr;
    Legionfall::JobSystem* g_jobSystem = nullptr;
    Legionfall::InputState g_input{};
    bool g_running = true;
    bool g_minimized = false;
    uint32_t g_width = 1280, g_height = 720;
    const wchar_t* CLASS_NAME = L"LegionfallWindow";
}

// Helper to show errors
void ShowError(const char* msg) {
    MessageBoxA(nullptr, msg, "Legionfall Error", MB_OK | MB_ICONERROR);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE: g_running = false; return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_SIZE: {
            uint32_t w = LOWORD(lParam), h = HIWORD(lParam);
            g_minimized = (w == 0 || h == 0);
            if (!g_minimized) { 
                g_width = w; g_height = h; 
                if (g_renderer) g_renderer->onResize(w, h); 
            }
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
    // Always create console for debugging
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    
    std::cout << "========================================" << std::endl;
    std::cout << "    Legionfall - Phase 0 Starting      " << std::endl;
    std::cout << "========================================" << std::endl;

    // Register window class
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASS_NAME;
    
    if (!RegisterClassExW(&wc)) {
        ShowError("Failed to register window class!");
        return 1;
    }
    std::cout << "[OK] Window class registered" << std::endl;

    // Calculate window size
    RECT r = {0, 0, (LONG)g_width, (LONG)g_height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = r.right - r.left;
    int wh = r.bottom - r.top;
    
    // Create window
    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"Legionfall - Phase 0",
        WS_OVERLAPPEDWINDOW,
        (sw - ww) / 2, (sh - wh) / 2,
        ww, wh,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        ShowError("Failed to create window!");
        return 1;
    }
    std::cout << "[OK] Window created (" << g_width << "x" << g_height << ")" << std::endl;

    // Create subsystems
    try {
        g_jobSystem = new Legionfall::JobSystem();
        std::cout << "[OK] JobSystem created with " << g_jobSystem->threadCount() << " worker threads" << std::endl;
    } catch (const std::exception& e) {
        std::string err = std::string("JobSystem failed: ") + e.what();
        ShowError(err.c_str());
        return 1;
    }

    try {
        g_game = new Legionfall::Game();
        std::cout << "[OK] Game created" << std::endl;
    } catch (const std::exception& e) {
        std::string err = std::string("Game failed: ") + e.what();
        ShowError(err.c_str());
        return 1;
    }

    try {
        g_renderer = new Legionfall::Renderer();
        std::cout << "[OK] Renderer created" << std::endl;
    } catch (const std::exception& e) {
        std::string err = std::string("Renderer failed: ") + e.what();
        ShowError(err.c_str());
        return 1;
    }

    // Initialize Vulkan
    std::cout << "[..] Initializing Vulkan..." << std::endl;
    if (!g_renderer->init(hwnd, hInstance, g_width, g_height)) {
        ShowError("Failed to initialize Vulkan!\n\nMake sure you have:\n- Vulkan SDK installed\n- Updated graphics drivers");
        delete g_renderer;
        delete g_game;
        delete g_jobSystem;
        DestroyWindow(hwnd);
        return 1;
    }
    std::cout << "[OK] Vulkan initialized" << std::endl;

    // Initialize game
    g_game->init(100);
    std::cout << "[OK] Game initialized with 100 enemies" << std::endl;
    
    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    
    std::cout << "========================================" << std::endl;
    std::cout << " Phase 0 Ready! Window should be open. " << std::endl;
    std::cout << " Controls: WASD=Move, ESC=Exit         " << std::endl;
    std::cout << "========================================" << std::endl;

    // Main loop timing
    auto lastTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    double fpsTimer = 0.0;
    
    MSG msg{};
    while (g_running) {
        // Process messages
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        
        if (!g_running) break;

        // Calculate delta time
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;
        
        // Skip if minimized
        if (g_minimized) {
            Sleep(10);
            continue;
        }

        // Update game
        g_game->update(dt, g_input, g_jobSystem);
        
        // Update renderer
        g_renderer->updateInstanceBuffer(g_game->getInstanceData());
        g_renderer->drawFrame();
        
        // FPS counter
        frameCount++;
        fpsTimer += dt;
        if (fpsTimer >= 1.0) {
            std::cout << "FPS: " << frameCount << " | Enemies: " << g_game->getStats().enemyCount << std::endl;
            frameCount = 0;
            fpsTimer = 0.0;
        }
    }

    std::cout << "Shutting down..." << std::endl;

    // Cleanup
    delete g_renderer;
    delete g_game;
    delete g_jobSystem;
    DestroyWindow(hwnd);
    UnregisterClassW(CLASS_NAME, hInstance);

    std::cout << "Goodbye!" << std::endl;
    
    // Keep console open briefly
    Sleep(500);
    FreeConsole();
    
    return 0;
}