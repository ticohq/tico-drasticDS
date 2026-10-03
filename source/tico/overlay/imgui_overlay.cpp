// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include <switch.h>

#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_vulkan.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "drastic_overlay_hook.h"
extern "C" {
#include "tico/tico_entry.h"
}

#include "tico/overlay/imgui_overlay.h"

namespace SwitchFrontend::ImGuiOverlay {
namespace {

constexpr const char* TAG = "[tico-overlay]";
constexpr std::array<const char*, 3> kFontPaths = {{
    "romfs:/fonts/font.ttf",
    "sdmc:/tico/fonts/font.ttf",
    "sdmc:/tico/system/nds/fonts/font.ttf",
}};
constexpr std::array<const char*, 6> kAvatarPaths = {{
    "sdmc:/tico/assets/avatar.jpg",
    "sdmc:/tico/assets/avatar.jpeg",
    "sdmc:/tico/assets/avatar.png",
    "romfs:/assets/avatar.jpg",
    "romfs:/assets/avatar.jpeg",
    "romfs:/assets/avatar.png",
}};
// the overlay's layout is designed for a 720p display
constexpr float kDesignHeight = 720.0f;

bool s_initialized = false;
bool s_vulkan = false;
bool s_visible = false;
bool s_psm_initialized = false;
OverlayUI::NavInput s_nav{};
OverlayUI::Action s_action = OverlayUI::Action::None;

// The avatar, decoded once and uploaded when a backend is ready.
std::vector<unsigned char> s_avatar_rgba;
int s_avatar_width = 0;
int s_avatar_height = 0;
bool s_avatar_decoded = false;

bool DecodeAvatar(unsigned char* rgba, int width, int height, const char* source) {
    if (!rgba) {
        return false;
    }
    s_avatar_rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
    s_avatar_width = width;
    s_avatar_height = height;
    stbi_image_free(rgba);
    tico_log("%s loaded avatar: %s (%dx%d)\n", TAG, source, width, height);
    return true;
}

bool DecodeAvatarFromAccount() {
    if (R_FAILED(accountInitialize(AccountServiceType_Application))) {
        return false;
    }

    AccountUid uid{};
    bool found = R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid);
    if (!found) {
        found = R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid);
    }
    if (!found) {
        AccountUid uids[ACC_USER_LIST_SIZE]{};
        s32 total = 0;
        if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total)) && total > 0) {
            uid = uids[0];
            found = accountUidIsValid(&uid);
        }
    }

    bool decoded = false;
    AccountProfile profile{};
    if (found && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        AccountProfileBase profile_base{};
        if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profile_base)) &&
            profile_base.nickname[0] != '\0') {
            OverlayUI::SetNickname(profile_base.nickname);
        }
        u32 image_size = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &image_size)) && image_size > 0) {
            std::vector<unsigned char> jpeg(image_size);
            u32 actual = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), image_size, &actual)) &&
                actual > 0) {
                int width = 0;
                int height = 0;
                int channels = 0;
                decoded = DecodeAvatar(stbi_load_from_memory(jpeg.data(), static_cast<int>(actual),
                                                             &width, &height, &channels, 4),
                                       width, height, "switch-account-avatar");
            }
        }
        accountProfileClose(&profile);
    }
    accountExit();
    return decoded;
}

void DecodeAvatarOnce() {
    if (s_avatar_decoded) {
        return;
    }
    s_avatar_decoded = true;
    for (const char* path : kAvatarPaths) {
        int width = 0;
        int height = 0;
        int channels = 0;
        if (DecodeAvatar(stbi_load(path, &width, &height, &channels, 4), width, height, path)) {
            return;
        }
    }
    if (!DecodeAvatarFromAccount()) {
        tico_log("%s no avatar image found\n", TAG);
    }
}

void BeginFrame(float width, float height) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60.0f;
    io.FontGlobalScale = height / kDesignHeight;
    OverlayUI::FeedNav(s_nav);
    s_nav = {};
}

// Draws the menu or the HUD for this frame. Returns false when there is
// nothing to draw, so the renderer is left alone.
bool BuildFrame(float width, float height) {
    if (!s_visible && !OverlayUI::HasTransientContent()) {
        return false;
    }
    BeginFrame(width, height);
    ImGui::NewFrame();
    const OverlayUI::Action action =
        OverlayUI::Render(static_cast<int>(width), static_cast<int>(height));
    ImGui::Render();
    if (action != OverlayUI::Action::None) {
        s_action = action;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Vulkan (NVK)

struct VulkanState {
    bool ready = false;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkImage avatar_image = VK_NULL_HANDLE;
    VkDeviceMemory avatar_memory = VK_NULL_HANDLE;
    VkImageView avatar_view = VK_NULL_HANDLE;
    VkSampler avatar_sampler = VK_NULL_HANDLE;
    VkDescriptorSet avatar_descriptor = VK_NULL_HANDLE;
};
VulkanState s_vk;

bool FindMemoryType(u32 type_filter, VkMemoryPropertyFlags properties, u32& out_index) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(s_vk.physical_device, &memory);
    for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            out_index = i;
            return true;
        }
    }
    return false;
}

void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                     VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                     VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void DestroyVulkanAvatar() {
    if (s_vk.avatar_descriptor) {
        ImGui_ImplVulkan_RemoveTexture(s_vk.avatar_descriptor);
        s_vk.avatar_descriptor = VK_NULL_HANDLE;
    }
    if (s_vk.avatar_sampler)
        vkDestroySampler(s_vk.device, s_vk.avatar_sampler, nullptr);
    if (s_vk.avatar_view)
        vkDestroyImageView(s_vk.device, s_vk.avatar_view, nullptr);
    if (s_vk.avatar_image)
        vkDestroyImage(s_vk.device, s_vk.avatar_image, nullptr);
    if (s_vk.avatar_memory)
        vkFreeMemory(s_vk.device, s_vk.avatar_memory, nullptr);
    s_vk.avatar_sampler = VK_NULL_HANDLE;
    s_vk.avatar_view = VK_NULL_HANDLE;
    s_vk.avatar_image = VK_NULL_HANDLE;
    s_vk.avatar_memory = VK_NULL_HANDLE;
    OverlayUI::SetAvatarTextureId(0);
}

// Uploads the decoded avatar with a one-off submission. Runs before the
// renderer submits the frame being recorded, so the queue is free.
void UploadVulkanAvatar() {
    if (s_avatar_rgba.empty() || s_vk.avatar_descriptor) {
        return;
    }
    const u32 width = static_cast<u32>(s_avatar_width);
    const u32 height = static_cast<u32>(s_avatar_height);
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    auto finish = [&](const char* failure) {
        if (command)
            vkFreeCommandBuffers(s_vk.device, s_vk.command_pool, 1, &command);
        if (staging)
            vkDestroyBuffer(s_vk.device, staging, nullptr);
        if (staging_memory)
            vkFreeMemory(s_vk.device, staging_memory, nullptr);
        if (failure) {
            tico_log("%s avatar upload failed: %s\n", TAG, failure);
            DestroyVulkanAvatar();
            // do not retry every frame
            s_avatar_rgba.clear();
        }
    };

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(s_vk.device, &buffer_info, nullptr, &staging) != VK_SUCCESS)
        return finish("staging buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(s_vk.device, staging, &requirements);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    if (!FindMemoryType(requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        alloc.memoryTypeIndex))
        return finish("no host-visible memory");
    void* mapped = nullptr;
    if (vkAllocateMemory(s_vk.device, &alloc, nullptr, &staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(s_vk.device, staging, staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(s_vk.device, staging_memory, 0, size, 0, &mapped) != VK_SUCCESS)
        return finish("staging memory");
    std::memcpy(mapped, s_avatar_rgba.data(), static_cast<std::size_t>(size));
    vkUnmapMemory(s_vk.device, staging_memory);

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_vk.device, &image_info, nullptr, &s_vk.avatar_image) != VK_SUCCESS)
        return finish("image");
    vkGetImageMemoryRequirements(s_vk.device, s_vk.avatar_image, &requirements);
    alloc.allocationSize = requirements.size;
    if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        alloc.memoryTypeIndex))
        return finish("no device-local memory");
    if (vkAllocateMemory(s_vk.device, &alloc, nullptr, &s_vk.avatar_memory) != VK_SUCCESS ||
        vkBindImageMemory(s_vk.device, s_vk.avatar_image, s_vk.avatar_memory, 0) != VK_SUCCESS)
        return finish("image memory");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = s_vk.avatar_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s_vk.device, &view_info, nullptr, &s_vk.avatar_view) != VK_SUCCESS)
        return finish("image view");

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(s_vk.device, &sampler_info, nullptr, &s_vk.avatar_sampler) != VK_SUCCESS)
        return finish("sampler");

    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = s_vk.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(s_vk.device, &command_info, &command) != VK_SUCCESS)
        return finish("command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin);
    TransitionImage(command, s_vk.avatar_image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command, staging, s_vk.avatar_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImage(command, s_vk.avatar_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(command);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (vkQueueSubmit(s_vk.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
        return finish("queue submit");
    vkQueueWaitIdle(s_vk.queue);

    s_vk.avatar_descriptor = ImGui_ImplVulkan_AddTexture(
        s_vk.avatar_sampler, s_vk.avatar_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    OverlayUI::SetAvatarTextureId(
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s_vk.avatar_descriptor)));
    finish(nullptr);
}

void DestroyVulkanBackend() {
    if (s_vk.device) {
        vkDeviceWaitIdle(s_vk.device);
    }
    if (s_vk.ready) {
        DestroyVulkanAvatar();
        ImGui_ImplVulkan_Shutdown();
    }
    if (s_vk.descriptor_pool)
        vkDestroyDescriptorPool(s_vk.device, s_vk.descriptor_pool, nullptr);
    if (s_vk.command_pool)
        vkDestroyCommandPool(s_vk.device, s_vk.command_pool, nullptr);
    s_vk = {};
}

// The backend draws inside the renderer's final render pass, so it is built
// against that pass and rebuilt if the renderer ever hands over another one.
bool EnsureVulkanBackend(const DrasticVkOverlayContext& context) {
    if (s_vk.ready && s_vk.render_pass == context.render_pass && s_vk.device == context.device) {
        return true;
    }
    DestroyVulkanBackend();
    s_vk.physical_device = context.physical_device;
    s_vk.device = context.device;
    s_vk.queue = context.queue;
    s_vk.render_pass = context.render_pass;

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 64;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkCommandPoolCreateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_info.queueFamilyIndex = context.queue_family;
    if (vkCreateDescriptorPool(s_vk.device, &pool_info, nullptr, &s_vk.descriptor_pool) !=
            VK_SUCCESS ||
        vkCreateCommandPool(s_vk.device, &command_info, nullptr, &s_vk.command_pool) !=
            VK_SUCCESS) {
        tico_log("%s could not create the Vulkan pools\n", TAG);
        DestroyVulkanBackend();
        return false;
    }

    const u32 image_count = context.image_count >= 2 ? context.image_count : 2;
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_1;
    init.Instance = context.instance;
    init.PhysicalDevice = context.physical_device;
    init.Device = context.device;
    init.QueueFamily = context.queue_family;
    init.Queue = context.queue;
    init.DescriptorPool = s_vk.descriptor_pool;
    init.RenderPass = context.render_pass;
    init.MinImageCount = image_count;
    init.ImageCount = image_count;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.Subpass = 0;
    if (!ImGui_ImplVulkan_Init(&init)) {
        tico_log("%s ImGui_ImplVulkan_Init failed\n", TAG);
        DestroyVulkanBackend();
        return false;
    }
    s_vk.ready = true;
    tico_log("%s Vulkan backend ready (format=%d, images=%u)\n", TAG,
             static_cast<int>(context.format), image_count);
    return true;
}

void VulkanHook(const DrasticVkOverlayContext* context) {
    if (!s_initialized || !context) {
        return;
    }
    if (!s_visible && !OverlayUI::HasTransientContent()) {
        return;
    }
    if (!EnsureVulkanBackend(*context)) {
        return;
    }
    UploadVulkanAvatar();
    ImGui_ImplVulkan_NewFrame();
    if (BuildFrame(static_cast<float>(context->width), static_cast<float>(context->height))) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), context->command_buffer);
    }
}

// ---------------------------------------------------------------------------
// OpenGL ES (NVC0 and Zink)

bool s_gl_ready = false;
GLuint s_gl_avatar = 0;

void UploadGlAvatar() {
    if (s_avatar_rgba.empty() || s_gl_avatar) {
        return;
    }
    glGenTextures(1, &s_gl_avatar);
    glBindTexture(GL_TEXTURE_2D, s_gl_avatar);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_avatar_width, s_avatar_height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, s_avatar_rgba.data());
    OverlayUI::SetAvatarTextureId(static_cast<unsigned long long>(s_gl_avatar));
}

void GlHook(int width, int height) {
    if (!s_initialized) {
        return;
    }
    if (!s_visible && !OverlayUI::HasTransientContent()) {
        return;
    }
    if (!s_gl_ready) {
        // the host creates an OpenGL ES 2 context
        if (!ImGui_ImplOpenGL3_Init("#version 100")) {
            tico_log("%s ImGui_ImplOpenGL3_Init failed\n", TAG);
            return;
        }
        s_gl_ready = true;
        tico_log("%s OpenGL backend ready\n", TAG);
    }
    UploadGlAvatar();
    ImGui_ImplOpenGL3_NewFrame();
    if (BuildFrame(static_cast<float>(width), static_cast<float>(height))) {
        glViewport(0, 0, width, height);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

void DestroyGlBackend() {
    if (s_gl_avatar) {
        glDeleteTextures(1, &s_gl_avatar);
        s_gl_avatar = 0;
        OverlayUI::SetAvatarTextureId(0);
    }
    if (s_gl_ready) {
        ImGui_ImplOpenGL3_Shutdown();
        s_gl_ready = false;
    }
}

} // namespace

bool Init(bool vulkan) {
    if (s_initialized) {
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    for (const char* font_path : kFontPaths) {
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, 32.0f)) {
            io.FontDefault = font;
            tico_log("%s loaded font: %s\n", TAG, font_path);
            break;
        }
    }
    if (!io.FontDefault) {
        tico_log("%s could not load overlay font, using ImGui default\n", TAG);
    }
    ImGui::StyleColorsDark();

    if (!s_psm_initialized && R_SUCCEEDED(psmInitialize())) {
        s_psm_initialized = true;
    }
    DecodeAvatarOnce();

    s_vulkan = vulkan;
    s_visible = false;
    s_action = OverlayUI::Action::None;
    s_initialized = true;
    if (vulkan) {
        drastic_vk_overlay_hook = &VulkanHook;
    } else {
        drastic_gl_overlay_hook = &GlHook;
    }
    tico_log("%s initialized for %s\n", TAG, vulkan ? "Vulkan" : "OpenGL");
    return true;
}

void Shutdown() {
    if (!s_initialized) {
        return;
    }
    drastic_vk_overlay_hook = nullptr;
    drastic_gl_overlay_hook = nullptr;
    if (s_vulkan) {
        DestroyVulkanBackend();
    } else {
        DestroyGlBackend();
    }
    ImGui::DestroyContext();
    OverlayUI::SetVisible(false);
    OverlayUI::ShowToast(std::string{});
    if (s_psm_initialized) {
        psmExit();
        s_psm_initialized = false;
    }
    s_initialized = false;
}

void SetVisible(bool visible) {
    s_visible = visible;
    s_nav = {};
    OverlayUI::SetVisible(visible);
}

bool IsVisible() {
    return s_visible;
}

void FeedNav(const OverlayUI::NavInput& nav) {
    s_nav.up |= nav.up;
    s_nav.down |= nav.down;
    s_nav.left |= nav.left;
    s_nav.right |= nav.right;
    s_nav.accept |= nav.accept;
    s_nav.cancel |= nav.cancel;
}

OverlayUI::Action ConsumeAction() {
    const OverlayUI::Action action = s_action;
    s_action = OverlayUI::Action::None;
    return action;
}

} // namespace SwitchFrontend::ImGuiOverlay
