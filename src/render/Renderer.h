#pragma once
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <optional>

namespace Legionfall {

struct InstanceData;

class Renderer {
public:
    Renderer();
    ~Renderer();
    bool init(HWND hwnd, HINSTANCE hinstance, uint32_t width, uint32_t height);
    void cleanup();
    void onResize(uint32_t width, uint32_t height);
    void updateInstanceBuffer(const std::vector<InstanceData>& instances);
    bool drawFrame();
    bool isInitialized() const { return m_initialized; }

private:
    bool createInstance();
    bool createSurface(HWND hwnd, HINSTANCE hinstance);
    bool pickPhysicalDevice();
    
    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;
        bool isComplete() const { return graphicsFamily.has_value() && presentFamily.has_value(); }
    };
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);

    bool m_initialized = false;
    uint32_t m_width = 0, m_height = 0;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    QueueFamilyIndices m_queueFamilyIndices;
    uint32_t m_instanceCount = 0;
};

}
