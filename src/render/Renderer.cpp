#include "render/Renderer.h"
#include "core/Game.h"
#include <iostream>
#include <cstring>

#ifdef _DEBUG
#define LOG(msg) std::cerr << "[Renderer] " << msg << std::endl
#else
#define LOG(msg)
#endif

namespace Legionfall {

Renderer::Renderer() = default;
Renderer::~Renderer() { cleanup(); }

bool Renderer::init(HWND hwnd, HINSTANCE hinstance, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    LOG("Initializing Vulkan...");
    
    if (!createInstance()) { LOG("Failed to create instance"); return false; }
    if (!createSurface(hwnd, hinstance)) { LOG("Failed to create surface"); return false; }
    if (!pickPhysicalDevice()) { LOG("Failed to find GPU"); return false; }
    
    LOG("Vulkan initialized successfully!");
    m_initialized = false;  // Will be true after Phase 1
    return true;
}

void Renderer::cleanup() {
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
    m_initialized = false;
}

void Renderer::onResize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
}

void Renderer::updateInstanceBuffer(const std::vector<InstanceData>& instances) {
    m_instanceCount = (uint32_t)instances.size();
    (void)instances;
}

bool Renderer::drawFrame() { return true; }

bool Renderer::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Legionfall";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "Legionfall Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    const char* extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;

#ifdef _DEBUG
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> available(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, available.data());
    for (const auto& l : available) {
        if (strcmp(l.layerName, layers[0]) == 0) {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = layers;
            LOG("Validation layers enabled");
            break;
        }
    }
#endif

    return vkCreateInstance(&createInfo, nullptr, &m_instance) == VK_SUCCESS;
}

bool Renderer::createSurface(HWND hwnd, HINSTANCE hinstance) {
    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hwnd = hwnd;
    createInfo.hinstance = hinstance;
    return vkCreateWin32SurfaceKHR(m_instance, &createInfo, nullptr, &m_surface) == VK_SUCCESS;
}

bool Renderer::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) return false;
    
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
    
    for (const auto& device : devices) {
        auto indices = findQueueFamilies(device);
        if (indices.graphicsFamily.has_value()) {
            m_physicalDevice = device;
            m_queueFamilyIndices = indices;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            LOG("Selected GPU: " << props.deviceName);
            return true;
        }
    }
    return false;
}

Renderer::QueueFamilyIndices Renderer::findQueueFamilies(VkPhysicalDevice device) {
    QueueFamilyIndices indices;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphicsFamily = i;
        VkBool32 present = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
        if (present) indices.presentFamily = i;
        if (indices.isComplete()) break;
    }
    return indices;
}

}
