// Runs isolated before/after EASU compute wrappers through software Vulkan.
// No game, window, swapchain, or production shader cache is used.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "renut_engine/gpu/render/easu_constants.h"

static void Check(VkResult result) {
  if (result != VK_SUCCESS) {
    std::fprintf(stderr, "Vulkan error %d\n", result);
    std::exit(1);
  }
}
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void* data{}; };
struct Push {
  float scale[2] = {1, 1}, translate[2] = {};
  uint32_t texture = 0, sampler = 0;
  float width = 0, height = 0;
  renut::gpu::EasuConstants constants{};
};
static_assert(sizeof(Push) == 96);

struct Harness {
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t queue_index{};
  VkPhysicalDeviceMemoryProperties memory_properties{};
  VkCommandPool commands{};
  VkDescriptorSetLayout layouts[3]{};
  VkPipelineLayout pipeline_layout{};
  VkPipeline pipelines[2]{};
  VkSampler sampler{};

  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    std::fprintf(stderr, "No suitable memory type\n"); std::exit(1);
  }
  Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo desc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    desc.size = size; desc.usage = usage; desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device, &desc, nullptr, &b.buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = req.size;
    allocation.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device, &allocation, nullptr, &b.memory));
    Check(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    Check(vkMapMemory(device, b.memory, 0, size, 0, &b.data));
    return b;
  }
  void Destroy(Buffer b) {
    vkUnmapMemory(device, b.memory); vkDestroyBuffer(device, b.buffer, nullptr); vkFreeMemory(device, b.memory, nullptr);
  }
  VkCommandBuffer Begin() {
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = commands; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
    VkCommandBuffer cmd; Check(vkAllocateCommandBuffers(device, &allocation, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; Check(vkBeginCommandBuffer(cmd, &begin));
    return cmd;
  }
  void Submit(VkCommandBuffer cmd) {
    Check(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); Check(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, commands, 1, &cmd);
  }
  Harness(char** paths) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "ME2 isolated EASU test"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
    Check(vkCreateInstance(&create, nullptr, &instance));
    uint32_t count = 0; Check(vkEnumeratePhysicalDevices(instance, &count, nullptr)); assert(count);
    std::vector<VkPhysicalDevice> devices(count); Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    physical = devices[0]; VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical, &properties);
    std::printf("Isolated shader test device: %s\n", properties.deviceName);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (queue_index = 0; queue_index < count; ++queue_index) if (families[queue_index].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
    assert(queue_index < count);
    float priority = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex = queue_index; q.queueCount = 1; q.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures available; vkGetPhysicalDeviceFeatures(physical, &available);
    assert(available.shaderImageGatherExtended);
    VkPhysicalDeviceFeatures features{}; features.shaderImageGatherExtended = true;
    VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; d.queueCreateInfoCount = 1; d.pQueueCreateInfos = &q; d.pEnabledFeatures = &features;
    Check(vkCreateDevice(physical, &d, nullptr, &device)); vkGetDeviceQueue(device, queue_index, 0, &queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = queue_index;
    Check(vkCreateCommandPool(device, &pool, nullptr, &commands));
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    for (unsigned i = 0; i < 3; ++i) {
      VkDescriptorSetLayoutBinding binding{}; binding.binding = 0; binding.descriptorType = types[i]; binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      VkDescriptorSetLayoutCreateInfo desc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; desc.bindingCount = 1; desc.pBindings = &binding;
      Check(vkCreateDescriptorSetLayout(device, &desc, nullptr, &layouts[i]));
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 3; layout.pSetLayouts = layouts; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(device, &layout, nullptr, &pipeline_layout));
    for (unsigned i = 0; i < 2; ++i) {
      std::ifstream input(paths[i], std::ios::binary | std::ios::ate); assert(input);
      size_t size = input.tellg(); assert(size % 4 == 0); std::vector<uint32_t> code(size / 4); input.seekg(0); input.read(reinterpret_cast<char*>(code.data()), size);
      VkShaderModuleCreateInfo desc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; desc.codeSize = size; desc.pCode = code.data();
      VkShaderModule module; Check(vkCreateShaderModule(device, &desc, nullptr, &module));
      VkComputePipelineCreateInfo pipe{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pipe.layout = pipeline_layout;
      pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipe.stage.module = module; pipe.stage.pName = "main";
      Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr, &pipelines[i])); vkDestroyShaderModule(device, module, nullptr);
    }
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; s.magFilter = s.minFilter = VK_FILTER_LINEAR; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; s.maxLod = 0;
    Check(vkCreateSampler(device, &s, nullptr, &sampler));
  }
  ~Harness() {
    vkDestroySampler(device, sampler, nullptr);
    for (auto p : pipelines) vkDestroyPipeline(device, p, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    for (auto l : layouts) vkDestroyDescriptorSetLayout(device, l, nullptr);
    vkDestroyCommandPool(device, commands, nullptr); vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
  }

  void Case(uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h, unsigned pattern, bool hdr = false) {
    auto input = MakeBuffer(size_t(w) * h * 4 * (hdr ? sizeof(float) : 1), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* pixels = static_cast<uint8_t*>(input.data);
    uint32_t rng = 1234567;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
      size_t at = (size_t(y) * w + x) * 4; rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
      const uint8_t rgba[] = {
          pattern == 0 ? uint8_t((x * 255) / std::max(1u, w - 1)) : uint8_t((x ^ y) & 1 ? 255 : 0),
          pattern == 0 ? uint8_t((y * 255) / std::max(1u, h - 1)) : uint8_t(rng),
          pattern == 0 ? uint8_t(x + y < (w + h) / 2 ? 255 : 0) : uint8_t(rng >> 8),
          uint8_t(rng >> 16)}; // Preserve independent, non-opaque alpha.
      for (unsigned c = 0; c < 4; ++c) {
        if (hdr) static_cast<float*>(input.data)[at + c] = float(rgba[c]) / 255.f * (c == 3 ? 1.f : 8.f);
        else pixels[at + c] = rgba[c];
      }
    }
    VkImageCreateInfo desc{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; desc.imageType = VK_IMAGE_TYPE_2D;
    desc.format = hdr ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    desc.extent = {w, h, 1}; desc.mipLevels = desc.arrayLayers = 1; desc.samples = VK_SAMPLE_COUNT_1_BIT;
    desc.tiling = VK_IMAGE_TILING_OPTIMAL; desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image; Check(vkCreateImage(device, &desc, nullptr, &image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = req.size;
    allocation.memoryTypeIndex = MemoryType(req.memoryTypeBits, 0);
    VkDeviceMemory memory; Check(vkAllocateMemory(device, &allocation, nullptr, &memory)); Check(vkBindImageMemory(device, image, memory, 0));
    VkImageViewCreateInfo view_desc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view_desc.image = image; view_desc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_desc.format = desc.format; view_desc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view; Check(vkCreateImageView(device, &view_desc, nullptr, &view));
    const size_t floats = size_t(out_w) * out_h * 4;
    Buffer outputs[] = {MakeBuffer(floats * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), MakeBuffer(floats * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)};
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo pool_desc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool_desc.maxSets = 4; pool_desc.poolSizeCount = 3; pool_desc.pPoolSizes = sizes;
    VkDescriptorPool pool; Check(vkCreateDescriptorPool(device, &pool_desc, nullptr, &pool));
    VkDescriptorSetLayout allocated_layouts[] = {layouts[0], layouts[1], layouts[2], layouts[2]};
    VkDescriptorSetAllocateInfo sets_desc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sets_desc.descriptorPool = pool; sets_desc.descriptorSetCount = 4; sets_desc.pSetLayouts = allocated_layouts;
    VkDescriptorSet sets[4]; Check(vkAllocateDescriptorSets(device, &sets_desc, sets));
    VkDescriptorImageInfo image_info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sampler_info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkDescriptorBufferInfo buffer_info[] = {{outputs[0].buffer, 0, floats * 4}, {outputs[1].buffer, 0, floats * 4}};
    VkWriteDescriptorSet writes[4]{};
    for (unsigned i = 0; i < 4; ++i) { writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = sets[i]; writes[i].descriptorCount = 1; }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].pImageInfo = &image_info;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; writes[1].pImageInfo = &sampler_info;
    for (unsigned i = 0; i < 2; ++i) { writes[i + 2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i + 2].pBufferInfo = &buffer_info[i]; }
    vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    auto cmd = Begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image; barrier.subresourceRange = view_desc.subresourceRange;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = desc.extent;
    vkCmdCopyBufferToImage(cmd, input.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    Push push; push.width = float(out_w); push.height = float(out_h); push.constants = renut::gpu::MakeEasuConstants(w, h, out_w, out_h);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 2, sets, 0, nullptr);
    for (unsigned i = 0; i < 2; ++i) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 2, 1, &sets[i + 2], 0, nullptr);
      vkCmdDispatch(cmd, (out_w + 7) / 8, (out_h + 7) / 8, 1);
    }
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    Submit(cmd);
    const float* a = static_cast<float*>(outputs[0].data); const float* b = static_cast<float*>(outputs[1].data);
    double maximum = 0, sum = 0; uint64_t quantized_differences = 0, alpha_differences = 0;
    for (size_t i = 0; i < floats; ++i) {
      assert(std::isfinite(a[i]) && std::isfinite(b[i]));
      double error = std::abs(double(a[i]) - double(b[i])); maximum = std::max(maximum, error); sum += error;
      if (std::lround(std::clamp(a[i], 0.f, 1.f) * 255) != std::lround(std::clamp(b[i], 0.f, 1.f) * 255)) ++quantized_differences;
      if (i % 4 == 3 && a[i] != b[i]) ++alpha_differences;
    }
    std::printf("%ux%u -> %ux%u %s pattern %u: max %.9g mean %.9g, changed RGBA8 channels %llu/%zu, alpha differences %llu\n",
                w, h, out_w, out_h, hdr ? "HDR" : "LDR", pattern, maximum, sum / floats, (unsigned long long)quantized_differences, floats, (unsigned long long)alpha_differences);
    std::fflush(stdout);
    assert(maximum == 0 && quantized_differences == 0 && alpha_differences == 0);
    vkDestroyDescriptorPool(device, pool, nullptr); for (auto b : outputs) Destroy(b);
    vkDestroyImageView(device, view, nullptr); vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr); Destroy(input);
  }
};

int main(int argc, char** argv) {
  assert(argc == 3); Harness harness(argv + 1);
  const uint32_t sizes[][4] = {{17, 9, 31, 17}, {640, 360, 1280, 720}, {853, 480, 1280, 720},
                              {640, 360, 1920, 1080}, {1280, 720, 1920, 1080}, {1280, 720, 1280, 720}};
  for (const auto& size : sizes) for (unsigned pattern = 0; pattern < 2; ++pattern)
    harness.Case(size[0], size[1], size[2], size[3], pattern);
  for (unsigned pattern = 0; pattern < 2; ++pattern) {
    harness.Case(853, 480, 1280, 720, pattern, true);
    harness.Case(1280, 720, 1920, 1080, pattern, true);
  }
  std::puts("PASS: isolated old/new EASU image comparisons; alpha identical");
}
