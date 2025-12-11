#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "render/Renderer.h"
#include "core/Game.h"
#include "core/JobSystem.h"
#include <chrono>
#include <iostream>
#include <iomanip>

namespace {
    Legionfall::Renderer* g_renderer = nullptr;
    Legionfall::Game* g_game = nullptr;
    Legionfall::JobSystem* g_jobSystem = nullptr;
    Legionfall::InputState g_input{};
    bool g_running = true;
    bool g_minimized = false;
    uint32_t g_width = 1280, g_height = 720;
    const wchar_t* CLASS_NAME = L"LegionfallWindow";

    constexpr uint32_t INITIAL_ENEMIES = 5000;
    bool g_gameOverShown = false;
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
                case 'C': g_input.toggleCameraFollow = true; break;
                case 'T': g_input.toggleChaseMode = true; break;
                case 'R': g_input.restart = true; break;
                case VK_OEM_PLUS: case VK_ADD: g_input.increaseEnemies = true; break;
                case VK_OEM_MINUS: case VK_SUBTRACT: g_input.decreaseEnemies = true; break;
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
                case 'C': g_input.toggleCameraFollow = false; break;
                case 'T': g_input.toggleChaseMode = false; break;
                case 'R': g_input.restart = false; break;
                case VK_OEM_PLUS: case VK_ADD: g_input.increaseEnemies = false; break;
                case VK_OEM_MINUS: case VK_SUBTRACT: g_input.decreaseEnemies = false; break;
            }
            return 0;
        default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void UpdateWindowTitle(HWND hwnd, const Legionfall::ProfilingStats& stats, int fps) {
    wchar_t title[512];
    swprintf_s(title, 
        L"LEGIONFALL | HP: %d | Kills: %u | Wave: %d | FPS: %d | Enemies: %u | %s | %s",
        stats.heroHealth,
        stats.killCount,
        stats.waveNumber,
        fps,
        stats.aliveCount,
        stats.parallelEnabled ? L"PARALLEL" : L"SINGLE",
        stats.chaseModeEnabled ? L"COMBAT" : L"PEACEFUL");
    SetWindowTextW(hwnd, title);
}

void PrintBanner() {
    std::cout << R"(
  _                _              __       _ _ 
 | |    ___  __ _ (_) ___  _ __  / _| __ _| | |
 | |   / _ \/ _` || |/ _ \| '_ \| |_ / _` | | |
 | |__|  __/ (_| || | (_) | | | |  _| (_| | | |
 |_____\___|\__, ||_|\___/|_| |_|_|  \__,_|_|_|
            |___/                               
    Micro-Strategy Arena - No Engine Required
)" << std::endl;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    AllocConsole();
    FILE* fp; freopen_s(&fp, "CONOUT$", "w", stdout); freopen_s(&fp, "CONOUT$", "w", stderr);
    
    PrintBanner();
    
    std::cout << "================================================" << std::endl;
    std::cout << " Initializing...                                " << std::endl;
    std::cout << "================================================" << std::endl;

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc; wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    RECT r = {0, 0, (LONG)g_width, (LONG)g_height}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"LEGIONFALL", WS_OVERLAPPEDWINDOW,
        (sw - (r.right - r.left)) / 2, (sh - (r.bottom - r.top)) / 2,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInstance, nullptr);

    g_jobSystem = new Legionfall::JobSystem();
    g_game = new Legionfall::Game();
    g_renderer = new Legionfall::Renderer();
    
    std::cout << " [+] JobSystem: " << g_jobSystem->threadCount() << " worker threads" << std::endl;

    if (!g_renderer->init(hwnd, hInstance, g_width, g_height)) {
        MessageBoxW(hwnd, L"Vulkan initialization failed!", L"Error", MB_OK);
        return 1;
    }

    g_game->init(INITIAL_ENEMIES);
    std::cout << " [+] Spawned " << INITIAL_ENEMIES << " enemies" << std::endl;
    
    ShowWindow(hwnd, nCmdShow);
    SetForegroundWindow(hwnd);
    
    std::cout << "================================================" << std::endl;
    std::cout << " CONTROLS:                                      " << std::endl;
    std::cout << "   WASD / Arrows  = Move                        " << std::endl;
    std::cout << "   SPACE          = Shockwave Attack            " << std::endl;
    std::cout << "   R              = Restart Game                " << std::endl;
    std::cout << "   +/-            = Adjust Enemy Count          " << std::endl;
    std::cout << "   T              = Toggle Combat/Peaceful      " << std::endl;
    std::cout << "   C              = Toggle Camera Follow        " << std::endl;
    std::cout << "   P              = Toggle Parallel/Single      " << std::endl;
    std::cout << "   H              = Toggle Heavy Work Mode      " << std::endl;
    std::cout << "   ESC            = Exit                        " << std::endl;
    std::cout << "================================================" << std::endl;
    std::cout << std::endl;
    std::cout << ">>> SURVIVE THE SWARM! PRESS SPACE TO ATTACK! <<<" << std::endl;
    std::cout << std::endl;

    using Clock = std::chrono::high_resolution_clock;
    auto lastTime = Clock::now();
    auto lastPrintTime = Clock::now();
    int frameCount = 0;
    double frameTimeAccum = 0.0;
    
    float cameraX = 0.0f, cameraY = 0.0f;
    
    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!g_running) break;

        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;
        if (g_minimized) { Sleep(10); continue; }

        // Handle restart
        if (g_input.restart && g_game->isGameOver()) {
            g_game->restart();
            g_gameOverShown = false;
            std::cout << std::endl << ">>> GAME RESTARTED! <<<" << std::endl << std::endl;
        }

        g_game->update(dt, g_input, g_jobSystem);
        
        // Camera
        float heroX, heroY;
        g_game->getHeroPosition(heroX, heroY);
        
        if (g_game->isCameraFollowEnabled()) {
            float followSpeed = 5.0f;
            cameraX += (heroX - cameraX) * followSpeed * dt;
            cameraY += (heroY - cameraY) * followSpeed * dt;
        } else {
            cameraX += (0.0f - cameraX) * 3.0f * dt;
            cameraY += (0.0f - cameraY) * 3.0f * dt;
        }
        
        g_renderer->setCameraPosition(cameraX, cameraY);
        g_renderer->updateInstanceBuffer(g_game->getInstanceData());
        g_renderer->drawFrame();

        frameCount++;
        frameTimeAccum += dt * 1000.0;

        double timeSincePrint = std::chrono::duration<double>(now - lastPrintTime).count();
        if (timeSincePrint >= 1.0) {
            auto& stats = g_game->getStats();
            int fps = frameCount;
            double avgFrameTime = frameTimeAccum / frameCount;
            
            std::cout << std::fixed << std::setprecision(2);
            
            if (stats.heroHealth > 0) {
                std::cout << "HP:" << std::setw(3) << stats.heroHealth 
                          << " | Kills:" << std::setw(5) << stats.killCount
                          << " | Wave:" << std::setw(2) << stats.waveNumber
                          << " | FPS:" << std::setw(4) << fps 
                          << " | " << std::setw(5) << stats.updateTimeMs << "ms"
                          << " | " << stats.aliveCount << " alive"
                          << " | " << (stats.parallelEnabled ? "PAR" : "SEQ")
                          << "(" << stats.threadCount << ")"
                          << std::endl;
            }
            
            UpdateWindowTitle(hwnd, stats, fps);
            
            frameCount = 0;
            frameTimeAccum = 0.0;
            lastPrintTime = now;
        }
        
        // Game over handling
        auto& stats = g_game->getStats();
        if (stats.heroHealth <= 0 && !g_gameOverShown) {
            g_gameOverShown = true;
            std::cout << std::endl;
            std::cout << "================================================" << std::endl;
            std::cout << "              *** GAME OVER ***                 " << std::endl;
            std::cout << "          Final Kill Count: " << std::setw(5) << stats.killCount << std::endl;
            std::cout << "          Wave Reached: " << std::setw(2) << stats.waveNumber << std::endl;
            std::cout << "                                                " << std::endl;
            std::cout << "          Press R to Restart                    " << std::endl;
            std::cout << "          Press ESC to Exit                     " << std::endl;
            std::cout << "================================================" << std::endl;
        }
    }

    std::cout << std::endl;
    std::cout << "================================================" << std::endl;
    std::cout << " Thanks for playing LEGIONFALL!                 " << std::endl;
    std::cout << "================================================" << std::endl;
    
    delete g_renderer; delete g_game; delete g_jobSystem;
    DestroyWindow(hwnd);
    Sleep(500);
    FreeConsole();
    return 0;
}