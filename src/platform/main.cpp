#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR

#include <windows.h>
#include <vulkan/vulkan.h>

#include <vector>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <optional>
#include <set>
#include <queue>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <cstring>
#include <stdexcept>

#include "../core/JobSystem.h"

// ====================================================================================
// Global Win32
// ====================================================================================

HINSTANCE g_hInstance = nullptr;
HWND g_hWnd = nullptr;

// ====================================================================================
// Global Vulkan core objects
// ====================================================================================

VkInstance       g_instance        = VK_NULL_HANDLE;
VkSurfaceKHR     g_surface         = VK_NULL_HANDLE;
VkPhysicalDevice g_physicalDevice  = VK_NULL_HANDLE;
VkDevice         g_device          = VK_NULL_HANDLE;
VkQueue          g_graphicsQueue   = VK_NULL_HANDLE;
VkQueue          g_presentQueue    = VK_NULL_HANDLE;

// Swapchain
VkSwapchainKHR              g_swapchain             = VK_NULL_HANDLE;
std::vector<VkImage>        g_swapchainImages;
std::vector<VkImageView>    g_swapchainImageViews;
VkFormat                    g_swapchainImageFormat{};
VkExtent2D                  g_swapchainExtent{};

// Render pass, framebuffers, commands
VkRenderPass                    g_renderPass = VK_NULL_HANDLE;
std::vector<VkFramebuffer>      g_swapchainFramebuffers;
VkCommandPool                   g_commandPool = VK_NULL_HANDLE;
std::vector<VkCommandBuffer>    g_commandBuffers;

// Sync objects
const int MAX_FRAMES_IN_FLIGHT = 2;
std::vector<VkSemaphore> g_imageAvailableSemaphores;
std::vector<VkSemaphore> g_renderFinishedSemaphores;
std::vector<VkFence>     g_inFlightFences;
size_t                   g_currentFrame = 0;

// Graphics pipeline
VkPipelineLayout g_pipelineLayout   = VK_NULL_HANDLE;
VkPipeline       g_graphicsPipeline = VK_NULL_HANDLE;

// Vertex + instance buffers
VkBuffer       g_vertexBuffer         = VK_NULL_HANDLE;
VkDeviceMemory g_vertexBufferMemory   = VK_NULL_HANDLE;

VkBuffer       g_instanceBuffer       = VK_NULL_HANDLE;
VkDeviceMemory g_instanceBufferMemory = VK_NULL_HANDLE;

// ====================================================================================
// Vertex & Instance Data
// ====================================================================================

struct Vertex {
    float pos[2];
    float color[3];
};

struct InstanceData {
    float offset[2];
    float color[3];
};

// Base triangle geometry
const std::vector<Vertex> g_vertices = {
    { {  0.0f, -0.5f }, { 1.0f, 0.0f, 0.0f } },
    { {  0.5f,  0.5f }, { 0.0f, 1.0f, 0.0f } },
    { { -0.5f,  0.5f }, { 0.0f, 0.0f, 1.0f } }
};

// Grid of instances (stress test)
const uint32_t INSTANCE_GRID_X = 200;
const uint32_t INSTANCE_GRID_Y = 200;
const uint32_t INSTANCE_COUNT  = INSTANCE_GRID_X * INSTANCE_GRID_Y;

std::vector<InstanceData> g_instances(INSTANCE_COUNT);
float g_time = 0.0f;

// ====================================================================================
// Profiling state
// ====================================================================================

using Clock = std::chrono::high_resolution_clock;

double g_accumTime   = 0.0;
int    g_accumFrames = 0;

double g_lastUpdateMs = 0.0;
double g_lastFrameMs  = 0.0;

// Toggle to compare single-threaded vs parallel updates
bool g_useParallelUpdate = true;

// Toggle to add a heavier per-instance CPU workload
bool g_useHeavyWork = false;


// ====================================================================================
// Helper structs for Vulkan
// ====================================================================================

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR      capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>  presentModes;
};

const std::vector<const char*> DEVICE_EXTENSIONS = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// ====================================================================================
// Job system handle
// ====================================================================================

std::unique_ptr<JobSystem> g_jobSystem;

// ====================================================================================
// Forward declarations
// ====================================================================================

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void createWin32Window(int width, int height, const char* title);
void initVulkan();
void initInstances();

void createInstance();
void createSurface();
void pickPhysicalDevice();
void createLogicalDevice();
void createSwapchain();
void createImageViews();
void createRenderPass();
void createGraphicsPipeline();
void createFramebuffers();
void createCommandPool();
void createVertexBuffer();
void createInstanceBuffer();
void createCommandBuffers();
void createSyncObjects();

void mainLoop();
void updateInstances(float dt);
void updateInstanceBuffer();
void drawFrame();
void cleanup();

QueueFamilyIndices      findQueueFamilies(VkPhysicalDevice device);
SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device);
VkSurfaceFormatKHR      chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
VkPresentModeKHR        chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes);
VkExtent2D              chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);

std::vector<char>   readFile(const std::string& filename);
VkShaderModule      createShaderModule(const std::vector<char>& code);
uint32_t            findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

// ====================================================================================
// Entry point
// ====================================================================================

int main() {
    g_hInstance = GetModuleHandle(nullptr);

    try {
        createWin32Window(800, 600, "Legionfall Vulkan (Crowd + Jobs + Profiling)");
        initVulkan();

        g_jobSystem = std::make_unique<JobSystem>();

        std::cout << "Press P in the window to toggle parallel update on/off.\n";

        mainLoop();

        vkDeviceWaitIdle(g_device);
        g_jobSystem.reset();
        cleanup();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        MessageBoxA(nullptr, e.what(), "Fatal error", MB_ICONERROR | MB_OK);
        return -1;
    }

    return 0;
}

// ====================================================================================
// Win32 window / message loop
// ====================================================================================

void createWin32Window(int width, int height, const char* title) {
    const char CLASS_NAME[] = "LegionfallWindowClass";

    WNDCLASSA wc{};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = g_hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);

    if (!RegisterClassA(&wc)) {
        throw std::runtime_error("Failed to register window class");
    }

    DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME);

    RECT rect{ 0, 0, width, height };
    AdjustWindowRect(&rect, style, FALSE);

    g_hWnd = CreateWindowExA(
        0,
        CLASS_NAME,
        title,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        g_hInstance,
        nullptr
    );

    if (!g_hWnd) {
        throw std::runtime_error("Failed to create window");
    }

    ShowWindow(g_hWnd, SW_SHOW);
}

void mainLoop() {
    MSG  msg{};
    bool running = true;

    auto lastPrint = Clock::now();

    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!running) break;

        auto frameStart = Clock::now();

        const float dt = 0.016f;

        auto updateStart = Clock::now();
        updateInstances(dt);
        auto updateEnd = Clock::now();

        updateInstanceBuffer();
        drawFrame();

        auto frameEnd = Clock::now();

        g_lastUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
        g_lastFrameMs  = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();

        double frameSeconds = std::chrono::duration<double>(frameEnd - frameStart).count();
        g_accumTime   += frameSeconds;
        g_accumFrames += 1;

        auto   now            = Clock::now();
        double sinceLastPrint = std::chrono::duration<double>(now - lastPrint).count();

        if (sinceLastPrint >= 1.0) {
            double fps = g_accumFrames / g_accumTime;
            std::cout << (g_useParallelUpdate ? "[Parallel] " : "[Single]  ")
                      << "FPS: " << fps
                      << " | update: " << g_lastUpdateMs << " ms"
                      << " | frame: " << g_lastFrameMs  << " ms"
                      << std::endl;

            g_accumFrames = 0;
            g_accumTime   = 0.0;
            lastPrint     = now;
        }

        if (GetAsyncKeyState('P') & 0x0001) {
            g_useParallelUpdate = !g_useParallelUpdate;
            std::cout << "Parallel update toggled to: "
                      << (g_useParallelUpdate ? "ON" : "OFF") << std::endl;
        }

        if (GetAsyncKeyState('H') & 0x0001) {
    g_useHeavyWork = !g_useHeavyWork;
    std::cout << "Heavy work mode toggled to: "
              << (g_useHeavyWork ? "ON" : "OFF") << std::endl;
        }

    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

// ====================================================================================
// Vulkan initialisation
// ====================================================================================

void initInstances() {
    const float span = 1.8f;

    for (uint32_t y = 0; y < INSTANCE_GRID_Y; ++y) {
        for (uint32_t x = 0; x < INSTANCE_GRID_X; ++x) {
            uint32_t idx = y * INSTANCE_GRID_X + x;

            float fx = (static_cast<float>(x) / (INSTANCE_GRID_X - 1)) - 0.5f;
            float fy = (static_cast<float>(y) / (INSTANCE_GRID_Y - 1)) - 0.5f;

            g_instances[idx].offset[0] = fx * span;
            g_instances[idx].offset[1] = fy * span;

            float t = static_cast<float>(idx) / static_cast<float>(INSTANCE_COUNT);
            g_instances[idx].color[0] = 0.5f + 0.5f * std::sin(t * 6.2831f);
            g_instances[idx].color[1] = 0.5f + 0.5f * std::sin(t * 6.2831f + 2.094f);
            g_instances[idx].color[2] = 0.5f + 0.5f * std::sin(t * 6.2831f + 4.188f);
        }
    }
}

void initVulkan() {
    createInstance();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createGraphicsPipeline();
    createFramebuffers();
    createCommandPool();
    createVertexBuffer();

    initInstances();
    createInstanceBuffer();

    createCommandBuffers();
    createSyncObjects();
}

// ====================================================================================
// Instance update (single-thread vs JobSystem parallel)
// ====================================================================================

void updateInstances(float dt) {
    g_time += dt;

    const uint32_t total = static_cast<uint32_t>(g_instances.size());
    if (total == 0) {
        return;
    }

    const float span = 1.8f;

    auto updateOne = [span](uint32_t i, float timeNow) {
        uint32_t x = i % INSTANCE_GRID_X;
        uint32_t y = i / INSTANCE_GRID_X;

        float fx = (static_cast<float>(x) / (INSTANCE_GRID_X - 1)) - 0.5f;
        float fy = (static_cast<float>(y) / (INSTANCE_GRID_Y - 1)) - 0.5f;

        float baseX = fx * span;
        float baseY = fy * span;

        float phase = static_cast<float>(i) * 0.15f;

        // Light work: a single trig pass
        float offsetX = baseX + 0.05f * std::sin(timeNow * 2.0f + phase);
        float offsetY = baseY + 0.05f * std::cos(timeNow * 2.0f + phase);

        if (g_useHeavyWork) {
            // Heavier CPU workload: do some extra iterative math on the same data.
            // This is intentionally branch-free and pure math to mimic a "compute" kernel.
            float accX = offsetX;
            float accY = offsetY;

            // 10 iterations of non-trivial trig; enough to see scaling,
            // not enough to freeze a 4090.
            for (int iter = 0; iter < 10; ++iter) {
                float t = timeNow * (1.0f + 0.05f * iter) + phase;
                float s = std::sin(t);
                float c = std::cos(t);

                accX = accX * 0.9f + 0.1f * (baseX + 0.1f * s);
                accY = accY * 0.9f + 0.1f * (baseY + 0.1f * c);
            }

            offsetX = accX;
            offsetY = accY;
        }

        g_instances[i].offset[0] = offsetX;
        g_instances[i].offset[1] = offsetY;
    };

    const float timeNow = g_time;

    // Single-threaded path
    if (!g_useParallelUpdate || !g_jobSystem) {
        for (uint32_t i = 0; i < total; ++i) {
            updateOne(i, timeNow);
        }
        return;
    }

    // Parallel path using JobSystem
    uint32_t threads = g_jobSystem->threadCount();
    if (threads == 0) threads = 1;

    const uint32_t chunkSize = (total + threads - 1) / threads;

    for (uint32_t t = 0; t < threads; ++t) {
        uint32_t begin = t * chunkSize;
        if (begin >= total) break;
        uint32_t end = std::min(begin + chunkSize, total);

        g_jobSystem->schedule([begin, end, timeNow, updateOne]() {
            for (uint32_t i = begin; i < end; ++i) {
                updateOne(i, timeNow);
            }
        });
    }

    g_jobSystem->wait();
}


void updateInstanceBuffer() {
    VkDeviceSize bufferSize = sizeof(InstanceData) * g_instances.size();

    void* data = nullptr;
    vkMapMemory(g_device, g_instanceBufferMemory, 0, bufferSize, 0, &data);
    std::memcpy(data, g_instances.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(g_device, g_instanceBufferMemory);
}

// ====================================================================================
// Vulkan instance + surface
// ====================================================================================

void createInstance() {
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "Legionfall";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "LegionfallEngine";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_2;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo        = &app;
    info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&info, nullptr, &g_instance) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance");
    }
}

void createSurface() {
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = g_hInstance;
    ci.hwnd      = g_hWnd;

    if (vkCreateWin32SurfaceKHR(g_instance, &ci, nullptr, &g_surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Win32 Vulkan surface");
    }
}

// ====================================================================================
// Device selection / queues
// ====================================================================================

QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    int i = 0;
    for (const auto& family : families) {
        if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = static_cast<uint32_t>(i);
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, g_surface, &presentSupport);
        if (presentSupport) {
            indices.presentFamily = static_cast<uint32_t>(i);
        }

        if (indices.isComplete()) {
            break;
        }

        ++i;
    }

    return indices;
}

SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) {
    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, g_surface, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, g_surface, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, g_surface, &formatCount, details.formats.data());
    }

    uint32_t presentCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, g_surface, &presentCount, nullptr);
    if (presentCount != 0) {
        details.presentModes.resize(presentCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, g_surface, &presentCount, details.presentModes.data());
    }

    return details;
}

bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);

    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    std::set<std::string> required(DEVICE_EXTENSIONS.begin(), DEVICE_EXTENSIONS.end());

    for (const auto& ext : available) {
        required.erase(ext.extensionName);
    }

    return required.empty();
}

bool isDeviceSuitable(VkPhysicalDevice device) {
    QueueFamilyIndices indices = findQueueFamilies(device);

    bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool swapchainAdequate = false;
    if (extensionsSupported) {
        SwapchainSupportDetails details = querySwapchainSupport(device);
        swapchainAdequate = !details.formats.empty() && !details.presentModes.empty();
    }

    return indices.isComplete() && extensionsSupported && swapchainAdequate;
}

void pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(g_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan-capable GPUs found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(g_instance, &deviceCount, devices.data());

    for (const auto& dev : devices) {
        if (isDeviceSuitable(dev)) {
            g_physicalDevice = dev;
            break;
        }
    }

    if (g_physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to find a suitable GPU");
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g_physicalDevice, &props);
    std::cout << "Using GPU: " << props.deviceName << std::endl;
}

void createLogicalDevice() {
    QueueFamilyIndices indices = findQueueFamilies(g_physicalDevice);

    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    std::set<uint32_t> uniqueFamilies = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()
    };

    float priority = 1.0f;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount       = 1;
        qi.pQueuePriorities = &priority;
        queueInfos.push_back(qi);
    }

    VkPhysicalDeviceFeatures features{};
    VkDeviceCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount    = static_cast<uint32_t>(queueInfos.size());
    info.pQueueCreateInfos       = queueInfos.data();
    info.pEnabledFeatures        = &features;
    info.enabledExtensionCount   = static_cast<uint32_t>(DEVICE_EXTENSIONS.size());
    info.ppEnabledExtensionNames = DEVICE_EXTENSIONS.data();

    if (vkCreateDevice(g_physicalDevice, &info, nullptr, &g_device) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create logical device");
    }

    vkGetDeviceQueue(g_device, indices.graphicsFamily.value(), 0, &g_graphicsQueue);
    vkGetDeviceQueue(g_device, indices.presentFamily.value(), 0, &g_presentQueue);
}

// ====================================================================================
// Swapchain + image views
// ====================================================================================

VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats[0];
}

VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) {
    for (const auto& m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            return m;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    RECT rc{};
    GetClientRect(g_hWnd, &rc);
    uint32_t width  = rc.right  - rc.left;
    uint32_t height = rc.bottom - rc.top;

    VkExtent2D actual{ width, height };

    // Manual clamp of width
    if (actual.width < capabilities.minImageExtent.width) {
        actual.width = capabilities.minImageExtent.width;
    } else if (actual.width > capabilities.maxImageExtent.width) {
        actual.width = capabilities.maxImageExtent.width;
    }

    // Manual clamp of height
    if (actual.height < capabilities.minImageExtent.height) {
        actual.height = capabilities.minImageExtent.height;
    } else if (actual.height > capabilities.maxImageExtent.height) {
        actual.height = capabilities.maxImageExtent.height;
    }

    return actual;
}


void createSwapchain() {
    SwapchainSupportDetails support = querySwapchainSupport(g_physicalDevice);

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    VkPresentModeKHR   presentMode   = chooseSwapPresentMode(support.presentModes);
    VkExtent2D         extent        = chooseSwapExtent(support.capabilities);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 &&
        imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface          = g_surface;
    info.minImageCount    = imageCount;
    info.imageFormat      = surfaceFormat.format;
    info.imageColorSpace  = surfaceFormat.colorSpace;
    info.imageExtent      = extent;
    info.imageArrayLayers = 1;
    info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices = findQueueFamilies(g_physicalDevice);
    uint32_t queueFamilyIndices[] = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()
    };

    if (indices.graphicsFamily != indices.presentFamily) {
        info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices   = queueFamilyIndices;
    }
    else {
        info.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
    }

    info.preTransform   = support.capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode    = presentMode;
    info.clipped        = VK_TRUE;
    info.oldSwapchain   = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(g_device, &info, nullptr, &g_swapchain) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swapchain");
    }

    vkGetSwapchainImagesKHR(g_device, g_swapchain, &imageCount, nullptr);
    g_swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &imageCount, g_swapchainImages.data());

    g_swapchainImageFormat = surfaceFormat.format;
    g_swapchainExtent      = extent;
}

void createImageViews() {
    g_swapchainImageViews.resize(g_swapchainImages.size());

    for (size_t i = 0; i < g_swapchainImages.size(); ++i) {
        VkImageViewCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image    = g_swapchainImages[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format   = g_swapchainImageFormat;
        info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel   = 0;
        info.subresourceRange.levelCount     = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(g_device, &info, nullptr, &g_swapchainImageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image view");
        }
    }
}

// ====================================================================================
// Render pass + pipeline
// ====================================================================================

void createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = g_swapchainImageFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments    = &colorAttachment;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dep;

    if (vkCreateRenderPass(g_device, &info, nullptr, &g_renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pass");
    }
}

std::vector<char> readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + filename);
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}

VkShaderModule createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    if (vkCreateShaderModule(g_device, &info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }

    return module;
}

void createGraphicsPipeline() {
    auto vertCode = readFile("../shaders/triangle.vert.spv");
    auto fragCode = readFile("../shaders/triangle.frag.spv");

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName  = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName  = "main";

    VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

    VkVertexInputBindingDescription bindings[2]{};

    bindings[0].binding   = 0;
    bindings[0].stride    = sizeof(Vertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    bindings[1].binding   = 1;
    bindings[1].stride    = sizeof(InstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[4]{};

    attributes[0].binding  = 0;
    attributes[0].location = 0;
    attributes[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset   = offsetof(Vertex, pos);

    attributes[1].binding  = 0;
    attributes[1].location = 1;
    attributes[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset   = offsetof(Vertex, color);

    attributes[2].binding  = 1;
    attributes[2].location = 2;
    attributes[2].format   = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset   = offsetof(InstanceData, offset);

    attributes[3].binding  = 1;
    attributes[3].location = 3;
    attributes[3].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[3].offset   = offsetof(InstanceData, color);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 2;
    vertexInput.pVertexBindingDescriptions      = bindings;
    vertexInput.vertexAttributeDescriptionCount = 4;
    vertexInput.pVertexAttributeDescriptions    = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(g_swapchainExtent.width);
    viewport.height   = static_cast<float>(g_swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = g_swapchainExtent;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports    = &viewport;
    viewportState.scissorCount  = 1;
    viewportState.pScissors     = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable        = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode             = VK_POLYGON_MODE_FILL;
    raster.lineWidth               = 1.0f;
    raster.cullMode                = VK_CULL_MODE_BACK_BIT;
    raster.frontFace               = VK_FRONT_FACE_CLOCKWISE;
    raster.depthBiasEnable         = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.sampleShadingEnable   = VK_FALSE;
    multisample.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable     = VK_FALSE;
    colorBlend.attachmentCount   = 1;
    colorBlend.pAttachments      = &colorBlendAttachment;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 0;
    layoutInfo.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(g_device, &layoutInfo, nullptr, &g_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_device, fragModule, nullptr);
        vkDestroyShaderModule(g_device, vertModule, nullptr);
        throw std::runtime_error("Failed to create pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState   = &multisample;
    pipelineInfo.pDepthStencilState  = nullptr;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = nullptr;
    pipelineInfo.layout              = g_pipelineLayout;
    pipelineInfo.renderPass          = g_renderPass;
    pipelineInfo.subpass             = 0;
    pipelineInfo.basePipelineHandle  = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_graphicsPipeline) != VK_SUCCESS) {
        vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
        vkDestroyShaderModule(g_device, fragModule, nullptr);
        vkDestroyShaderModule(g_device, vertModule, nullptr);
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    vkDestroyShaderModule(g_device, fragModule, nullptr);
    vkDestroyShaderModule(g_device, vertModule, nullptr);
}

// ====================================================================================
// Framebuffers + command pool/buffers
// ====================================================================================

void createFramebuffers() {
    g_swapchainFramebuffers.resize(g_swapchainImageViews.size());

    for (size_t i = 0; i < g_swapchainImageViews.size(); ++i) {
        VkImageView attachments[] = {
            g_swapchainImageViews[i]
        };

        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = g_renderPass;
        info.attachmentCount = 1;
        info.pAttachments    = attachments;
        info.width           = g_swapchainExtent.width;
        info.height          = g_swapchainExtent.height;
        info.layers          = 1;

        if (vkCreateFramebuffer(g_device, &info, nullptr, &g_swapchainFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create framebuffer");
        }
    }
}

void createCommandPool() {
    QueueFamilyIndices indices = findQueueFamilies(g_physicalDevice);

    VkCommandPoolCreateInfo info{};
    info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = indices.graphicsFamily.value();

    if (vkCreateCommandPool(g_device, &info, nullptr, &g_commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }
}

// ====================================================================================
// Buffers (vertex + instance)
// ====================================================================================

uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

void createVertexBuffer() {
    VkDeviceSize bufferSize = sizeof(g_vertices[0]) * g_vertices.size();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = bufferSize;
    bufferInfo.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(g_device, &bufferInfo, nullptr, &g_vertexBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex buffer");
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, g_vertexBuffer, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    if (vkAllocateMemory(g_device, &alloc, nullptr, &g_vertexBufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate vertex buffer memory");
    }

    void* data = nullptr;
    vkMapMemory(g_device, g_vertexBufferMemory, 0, bufferSize, 0, &data);
    std::memcpy(data, g_vertices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(g_device, g_vertexBufferMemory);

    vkBindBufferMemory(g_device, g_vertexBuffer, g_vertexBufferMemory, 0);
}

void createInstanceBuffer() {
    VkDeviceSize bufferSize = sizeof(InstanceData) * g_instances.size();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size        = bufferSize;
    bufferInfo.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(g_device, &bufferInfo, nullptr, &g_instanceBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create instance buffer");
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, g_instanceBuffer, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    if (vkAllocateMemory(g_device, &alloc, nullptr, &g_instanceBufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate instance buffer memory");
    }

    void* data = nullptr;
    vkMapMemory(g_device, g_instanceBufferMemory, 0, bufferSize, 0, &data);
    std::memcpy(data, g_instances.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(g_device, g_instanceBufferMemory);

    vkBindBufferMemory(g_device, g_instanceBuffer, g_instanceBufferMemory, 0);
}

// ====================================================================================
// Command buffers + sync
// ====================================================================================

void createCommandBuffers() {
    g_commandBuffers.resize(g_swapchainFramebuffers.size());

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool        = g_commandPool;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = static_cast<uint32_t>(g_commandBuffers.size());

    if (vkAllocateCommandBuffers(g_device, &alloc, g_commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }

    for (size_t i = 0; i < g_commandBuffers.size(); ++i) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        if (vkBeginCommandBuffer(g_commandBuffers[i], &begin) != VK_SUCCESS) {
            throw std::runtime_error("Failed to begin command buffer");
        }

        VkClearValue clear{};
        clear.color = { { 0.0f, 0.5f, 0.6f, 1.0f } };

        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = g_renderPass;
        rp.framebuffer       = g_swapchainFramebuffers[i];
        rp.renderArea.offset = { 0, 0 };
        rp.renderArea.extent = g_swapchainExtent;
        rp.clearValueCount   = 1;
        rp.pClearValues      = &clear;

        vkCmdBeginRenderPass(g_commandBuffers[i], &rp, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(g_commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, g_graphicsPipeline);

        VkBuffer     vertexBuffers[] = { g_vertexBuffer, g_instanceBuffer };
        VkDeviceSize offsets[]       = { 0, 0 };
        vkCmdBindVertexBuffers(g_commandBuffers[i], 0, 2, vertexBuffers, offsets);

        vkCmdDraw(
            g_commandBuffers[i],
            static_cast<uint32_t>(g_vertices.size()),
            static_cast<uint32_t>(g_instances.size()),
            0,
            0
        );

        vkCmdEndRenderPass(g_commandBuffers[i]);

        if (vkEndCommandBuffer(g_commandBuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to record command buffer");
        }
    }
}

void createSyncObjects() {
    g_imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    g_renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    g_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(g_device, &semInfo, nullptr, &g_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(g_device, &semInfo, nullptr, &g_renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(g_device, &fenceInfo, nullptr, &g_inFlightFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create sync objects");
        }
    }
}

// ====================================================================================
// Per-frame draw
// ====================================================================================

void drawFrame() {
    vkWaitForFences(g_device, 1, &g_inFlightFences[g_currentFrame], VK_TRUE, UINT64_MAX);
    vkResetFences(g_device, 1, &g_inFlightFences[g_currentFrame]);

    uint32_t imageIndex = 0;
    VkResult res = vkAcquireNextImageKHR(
        g_device,
        g_swapchain,
        UINT64_MAX,
        g_imageAvailableSemaphores[g_currentFrame],
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (res != VK_SUCCESS) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    VkSemaphore          waitSemaphores[]   = { g_imageAvailableSemaphores[g_currentFrame] };
    VkPipelineStageFlags waitStages[]       = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore          signalSemaphores[] = { g_renderFinishedSemaphores[g_currentFrame] };

    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = waitSemaphores;
    submit.pWaitDstStageMask    = waitStages;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &g_commandBuffers[imageIndex];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = signalSemaphores;

    if (vkQueueSubmit(g_graphicsQueue, 1, &submit, g_inFlightFences[g_currentFrame]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer");
    }

    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = signalSemaphores;
    present.swapchainCount     = 1;
    present.pSwapchains        = &g_swapchain;
    present.pImageIndices      = &imageIndex;

    res = vkQueuePresentKHR(g_presentQueue, &present);
    if (res != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }

    g_currentFrame = (g_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

// ====================================================================================
// Cleanup
// ====================================================================================

void cleanup() {
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (g_imageAvailableSemaphores[i]) {
            vkDestroySemaphore(g_device, g_imageAvailableSemaphores[i], nullptr);
        }
        if (g_renderFinishedSemaphores[i]) {
            vkDestroySemaphore(g_device, g_renderFinishedSemaphores[i], nullptr);
        }
        if (g_inFlightFences[i]) {
            vkDestroyFence(g_device, g_inFlightFences[i], nullptr);
        }
    }

    if (g_vertexBuffer) {
        vkDestroyBuffer(g_device, g_vertexBuffer, nullptr);
    }
    if (g_vertexBufferMemory) {
        vkFreeMemory(g_device, g_vertexBufferMemory, nullptr);
    }

    if (g_instanceBuffer) {
        vkDestroyBuffer(g_device, g_instanceBuffer, nullptr);
    }
    if (g_instanceBufferMemory) {
        vkFreeMemory(g_device, g_instanceBufferMemory, nullptr);
    }

    if (g_graphicsPipeline) {
        vkDestroyPipeline(g_device, g_graphicsPipeline, nullptr);
    }
    if (g_pipelineLayout) {
        vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
    }

    for (auto fb : g_swapchainFramebuffers) {
        vkDestroyFramebuffer(g_device, fb, nullptr);
    }

    if (g_renderPass) {
        vkDestroyRenderPass(g_device, g_renderPass, nullptr);
    }

    for (auto view : g_swapchainImageViews) {
        vkDestroyImageView(g_device, view, nullptr);
    }

    if (g_swapchain) {
        vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
    }

    if (g_commandPool) {
        vkDestroyCommandPool(g_device, g_commandPool, nullptr);
    }

    if (g_device) {
        vkDestroyDevice(g_device, nullptr);
    }

    if (g_surface) {
        vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    }

    if (g_instance) {
        vkDestroyInstance(g_instance, nullptr);
    }

    if (g_hWnd) {
        DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
    }
}
