#include "render/Renderer.h"
#include "core/Game.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <array>

#define LOG(msg) std::cout << "[Renderer] " << msg << std::endl

namespace Legionfall {

// Triangle vertices (unit triangle)
struct Vertex {
    float x, y;
};

static const std::vector<Vertex> TRIANGLE_VERTICES = {
    {  0.0f,  0.5f },   // Top
    { -0.4f, -0.5f },   // Bottom-left
    {  0.4f, -0.5f }    // Bottom-right
};

Renderer::Renderer() = default;
Renderer::~Renderer() { cleanup(); }

bool Renderer::init(HWND hwnd, HINSTANCE hinstance, uint32_t width, uint32_t height) {
    m_hwnd = hwnd;
    m_hinstance = hinstance;
    m_width = width;
    m_height = height;

    LOG("Initializing Vulkan with instancing support...");

    if (!createInstance()) { LOG("Failed: createInstance"); return false; }
    if (!createSurface(hwnd, hinstance)) { LOG("Failed: createSurface"); return false; }
    if (!pickPhysicalDevice()) { LOG("Failed: pickPhysicalDevice"); return false; }
    if (!createLogicalDevice()) { LOG("Failed: createLogicalDevice"); return false; }
    if (!createSwapchain()) { LOG("Failed: createSwapchain"); return false; }
    if (!createImageViews()) { LOG("Failed: createImageViews"); return false; }
    if (!createRenderPass()) { LOG("Failed: createRenderPass"); return false; }
    if (!createPipeline()) { LOG("Failed: createPipeline"); return false; }
    if (!createFramebuffers()) { LOG("Failed: createFramebuffers"); return false; }
    if (!createCommandPool()) { LOG("Failed: createCommandPool"); return false; }
    if (!createVertexBuffer()) { LOG("Failed: createVertexBuffer"); return false; }
    if (!createInstanceBuffer(10000)) { LOG("Failed: createInstanceBuffer"); return false; }
    if (!createCommandBuffers()) { LOG("Failed: createCommandBuffers"); return false; }
    if (!createSyncObjects()) { LOG("Failed: createSyncObjects"); return false; }

    m_initialized = true;
    LOG("Vulkan initialization complete with instancing!");
    return true;
}

void Renderer::cleanup() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }

    cleanupSwapchain();

    if (m_instanceBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_instanceBuffer, nullptr);
        vkFreeMemory(m_device, m_instanceBufferMemory, nullptr);
    }
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (m_renderFinishedSemaphores.size() > i)
            vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
        if (m_imageAvailableSemaphores.size() > i)
            vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
        if (m_inFlightFences.size() > i)
            vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
    }

    if (m_commandPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    if (m_device != VK_NULL_HANDLE)
        vkDestroyDevice(m_device, nullptr);
    if (m_surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_instance, nullptr);

    m_initialized = false;
}

void Renderer::cleanupSwapchain() {
    for (auto fb : m_framebuffers)
        vkDestroyFramebuffer(m_device, fb, nullptr);
    m_framebuffers.clear();

    if (m_graphicsPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_renderPass != VK_NULL_HANDLE)
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);

    for (auto iv : m_swapchainImageViews)
        vkDestroyImageView(m_device, iv, nullptr);
    m_swapchainImageViews.clear();

    if (m_swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);

    m_graphicsPipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_swapchain = VK_NULL_HANDLE;
}

void Renderer::onResize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    m_framebufferResized = true;
}

bool Renderer::recreateSwapchain() {
    if (m_width == 0 || m_height == 0) return true;
    vkDeviceWaitIdle(m_device);
    cleanupSwapchain();
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createPipeline()) return false;
    if (!createFramebuffers()) return false;
    return true;
}

void Renderer::updateInstanceBuffer(const std::vector<InstanceData>& instances) {
    if (instances.empty()) {
        m_instanceCount = 0;
        return;
    }

    size_t dataSize = instances.size() * sizeof(InstanceData);

    // Recreate buffer if needed
    if (dataSize > m_instanceBufferCapacity) {
        vkDeviceWaitIdle(m_device);
        if (m_instanceBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_instanceBuffer, nullptr);
            vkFreeMemory(m_device, m_instanceBufferMemory, nullptr);
        }
        createInstanceBuffer(instances.size() * 2);
    }

    // Upload data
    void* data;
    vkMapMemory(m_device, m_instanceBufferMemory, 0, dataSize, 0, &data);
    memcpy(data, instances.data(), dataSize);
    vkUnmapMemory(m_device, m_instanceBufferMemory);

    m_instanceCount = (uint32_t)instances.size();
}

bool Renderer::drawFrame() {
    if (!m_initialized || m_instanceCount == 0) return true;

    vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
        m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return true;
    }

    vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);
    vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);

    // Record command buffer
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo);

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = m_swapchainExtent;

    VkClearValue clearColor = {{{0.05f, 0.05f, 0.08f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_swapchainExtent.width;
    viewport.height = (float)m_swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_swapchainExtent;
    vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &scissor);

    // Push constants for orthographic view
    float aspect = (float)m_swapchainExtent.width / (float)m_swapchainExtent.height;
    PushConstants pc{};
    pc.viewScaleX = 1.0f / m_viewHalfWidth;
    pc.viewScaleY = aspect / m_viewHalfWidth;
    pc.viewOffsetX = m_cameraX;
    pc.viewOffsetY = m_cameraY;
    vkCmdPushConstants(m_commandBuffers[m_currentFrame], m_pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &pc);

    // Bind vertex buffer
    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(m_commandBuffers[m_currentFrame], 0, 1, vertexBuffers, offsets);

    // Bind instance buffer
    VkBuffer instanceBuffers[] = {m_instanceBuffer};
    vkCmdBindVertexBuffers(m_commandBuffers[m_currentFrame], 1, 1, instanceBuffers, offsets);

    // Draw all instances with one call!
    vkCmdDraw(m_commandBuffers[m_currentFrame], m_vertexCount, m_instanceCount, 0, 0);

    vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
    vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore waitSems[] = {m_imageAvailableSemaphores[m_currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSems;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];
    VkSemaphore signalSems[] = {m_renderFinishedSemaphores[m_currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSems;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]);

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSems;
    VkSwapchainKHR swapchains[] = {m_swapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
        m_framebufferResized = false;
        recreateSwapchain();
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    return true;
}

// === Vulkan Setup Functions ===

bool Renderer::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Legionfall";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Legionfall Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> available(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, available.data());
    for (const auto& l : available) {
        if (strcmp(l.layerName, layers[0]) == 0) {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = layers;
            break;
        }
    }

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
        m_queueFamilyIndices = findQueueFamilies(device);
        uint32_t extCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, exts.data());

        bool hasSwapchain = false;
        for (const auto& ext : exts) {
            if (strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                hasSwapchain = true;
                break;
            }
        }

        if (m_queueFamilyIndices.isComplete() && hasSwapchain) {
            auto support = querySwapchainSupport(device);
            if (!support.formats.empty() && !support.presentModes.empty()) {
                m_physicalDevice = device;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(device, &props);
                LOG("Selected GPU: " << props.deviceName);
                return true;
            }
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

    for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphicsFamily = i;
        VkBool32 present = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
        if (present) indices.presentFamily = i;
        if (indices.isComplete()) break;
    }
    return indices;
}

bool Renderer::createLogicalDevice() {
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::vector<uint32_t> uniqueFamilies;
    if (m_queueFamilyIndices.graphicsFamily.value() == m_queueFamilyIndices.presentFamily.value()) {
        uniqueFamilies.push_back(m_queueFamilyIndices.graphicsFamily.value());
    } else {
        uniqueFamilies.push_back(m_queueFamilyIndices.graphicsFamily.value());
        uniqueFamilies.push_back(m_queueFamilyIndices.presentFamily.value());
    }

    float priority = 1.0f;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        queueCreateInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceFeatures features{};
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = extensions;

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
        return false;

    vkGetDeviceQueue(m_device, m_queueFamilyIndices.graphicsFamily.value(), 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_queueFamilyIndices.presentFamily.value(), 0, &m_presentQueue);
    return true;
}

Renderer::SwapchainSupportDetails Renderer::querySwapchainSupport(VkPhysicalDevice device) {
    SwapchainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &details.capabilities);
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
    if (formatCount > 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, details.formats.data());
    }
    uint32_t modeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &modeCount, nullptr);
    if (modeCount > 0) {
        details.presentModes.resize(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &modeCount, details.presentModes.data());
    }
    return details;
}

VkSurfaceFormatKHR Renderer::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    return formats[0];
}

VkPresentModeKHR Renderer::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) {
    for (const auto& m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Renderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != UINT32_MAX) return capabilities.currentExtent;
    VkExtent2D extent = {m_width, m_height};
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

bool Renderer::createSwapchain() {
    auto support = querySwapchainSupport(m_physicalDevice);
    auto format = chooseSwapSurfaceFormat(support.formats);
    auto mode = chooseSwapPresentMode(support.presentModes);
    auto extent = chooseSwapExtent(support.capabilities);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount)
        imageCount = support.capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = format.format;
    createInfo.imageColorSpace = format.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t indices[] = {m_queueFamilyIndices.graphicsFamily.value(), m_queueFamilyIndices.presentFamily.value()};
    if (indices[0] != indices[1]) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = indices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = mode;
    createInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) return false;

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    m_swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());
    m_swapchainImageFormat = format.format;
    m_swapchainExtent = extent;
    return true;
}

bool Renderer::createImageViews() {
    m_swapchainImageViews.resize(m_swapchainImages.size());
    for (size_t i = 0; i < m_swapchainImages.size(); i++) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = m_swapchainImages[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = m_swapchainImageFormat;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device, &createInfo, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

bool Renderer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapchainImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = 1;
    createInfo.pAttachments = &colorAttachment;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    return vkCreateRenderPass(m_device, &createInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

std::vector<char> Renderer::readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) { LOG("Failed to open: " << filename); return {}; }
    size_t size = (size_t)file.tellg();
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

VkShaderModule Renderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

bool Renderer::createPipeline() {
    auto vertCode = readFile("shaders/instanced.vert.spv");
    auto fragCode = readFile("shaders/instanced.frag.spv");
    if (vertCode.empty() || fragCode.empty()) return false;

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);
    if (!vertModule || !fragModule) return false;

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    // Vertex input bindings
    std::array<VkVertexInputBindingDescription, 2> bindings{};
    // Binding 0: Per-vertex (position)
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(Vertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    // Binding 1: Per-instance
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(InstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    // Vertex attributes
    std::array<VkVertexInputAttributeDescription, 4> attributes{};
    // Location 0: Position (per-vertex)
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = 0;
    // Location 1: Offset (per-instance)
    attributes[1].binding = 1;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(InstanceData, offsetX);
    // Location 2: Color (per-instance)
    attributes[2].binding = 1;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[2].offset = offsetof(InstanceData, colorR);
    // Location 3: Scale (per-instance)
    attributes[3].binding = 1;
    attributes[3].location = 3;
    attributes[3].format = VK_FORMAT_R32_SFLOAT;
    attributes[3].offset = offsetof(InstanceData, scale);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = (uint32_t)bindings.size();
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = (uint32_t)attributes.size();
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // No culling for 2D
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = (uint32_t)dynamicStates.size();
    dynamicState.pDynamicStates = dynamicStates.data();

    // Push constant range
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline);
    vkDestroyShaderModule(m_device, vertModule, nullptr);
    vkDestroyShaderModule(m_device, fragModule, nullptr);

    if (result == VK_SUCCESS) LOG("Instanced pipeline created");
    return result == VK_SUCCESS;
}

bool Renderer::createFramebuffers() {
    m_framebuffers.resize(m_swapchainImageViews.size());
    for (size_t i = 0; i < m_swapchainImageViews.size(); i++) {
        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = m_renderPass;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = &m_swapchainImageViews[i];
        createInfo.width = m_swapchainExtent.width;
        createInfo.height = m_swapchainExtent.height;
        createInfo.layers = 1;
        if (vkCreateFramebuffer(m_device, &createInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

bool Renderer::createCommandPool() {
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = m_queueFamilyIndices.graphicsFamily.value();
    return vkCreateCommandPool(m_device, &createInfo, nullptr, &m_commandPool) == VK_SUCCESS;
}

bool Renderer::createCommandBuffers() {
    m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = (uint32_t)m_commandBuffers.size();
    return vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) == VK_SUCCESS;
}

bool Renderer::createSyncObjects() {
    m_imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(m_device, &semInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(m_device, &semInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

uint32_t Renderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return 0;
}

bool Renderer::createVertexBuffer() {
    VkDeviceSize bufferSize = sizeof(Vertex) * TRIANGLE_VERTICES.size();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vertexBufferMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexBufferMemory, 0);

    void* data;
    vkMapMemory(m_device, m_vertexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, TRIANGLE_VERTICES.data(), bufferSize);
    vkUnmapMemory(m_device, m_vertexBufferMemory);

    m_vertexCount = (uint32_t)TRIANGLE_VERTICES.size();
    return true;
}

bool Renderer::createInstanceBuffer(size_t capacity) {
    m_instanceBufferCapacity = capacity * sizeof(InstanceData);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = m_instanceBufferCapacity;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_instanceBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_instanceBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_instanceBufferMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, m_instanceBuffer, m_instanceBufferMemory, 0);

    LOG("Instance buffer created for " << capacity << " instances");
    return true;
}

}