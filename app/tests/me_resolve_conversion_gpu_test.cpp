// Actual fragment shader, render-target quantization and image copies on
// software Vulkan. No game, window, production cache or NRO is used.
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

static void Check(VkResult result) { if (result != VK_SUCCESS) { std::fprintf(stderr,"Vulkan error %d\n",result); std::exit(1); } }
struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; void *data{}; };
struct Image { VkImage handle{}; VkDeviceMemory memory{}; VkImageView view{}; VkFramebuffer frame{}; VkDescriptorSet set{}; VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED; };
struct Push { float scale[2]={1,1}, translate[2]={}; uint32_t texture=0,sampler=0; float param0=0,param1=0; };
static_assert(sizeof(Push)==32);
constexpr uint32_t W=269,H=251; // Odd extents, with more than 65,536 source texels.

struct Harness {
 VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
 uint32_t family=0; VkPhysicalDeviceMemoryProperties memory{}; VkCommandPool commands{};
 VkDescriptorSetLayout layouts[2]{}; VkPipelineLayout layout{}; VkShaderModule vertex{},pixel{};
 VkSampler sampler{}; VkDescriptorPool descriptors{}; VkDescriptorSet sampler_set{};
 VkRenderPass pass{}; VkPipeline pipeline{};
 bool unscaled_negative_control=false;
 uint32_t cases=0;
 uint32_t MemoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
  for(uint32_t i=0;i<memory.memoryTypeCount;++i) if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
  std::abort();
 }
 Buffer MakeBuffer(size_t bytes,VkBufferUsageFlags usage) {
  Buffer b; VkBufferCreateInfo desc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; desc.size=bytes; desc.usage=usage;
  Check(vkCreateBuffer(device,&desc,nullptr,&b.handle)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,b.handle,&req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize=req.size;
  alloc.memoryTypeIndex=MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  Check(vkAllocateMemory(device,&alloc,nullptr,&b.memory)); Check(vkBindBufferMemory(device,b.handle,b.memory,0));
  Check(vkMapMemory(device,b.memory,0,bytes,0,&b.data)); return b;
 }
 void Destroy(Buffer &b) { vkUnmapMemory(device,b.memory); vkDestroyBuffer(device,b.handle,nullptr); vkFreeMemory(device,b.memory,nullptr); }
 VkShaderModule Shader(const char *path) {
  std::ifstream file(path,std::ios::binary|std::ios::ate); assert(file); size_t size=file.tellg(); file.seekg(0);
  std::vector<uint32_t> code((size+3)/4); file.read(reinterpret_cast<char*>(code.data()),size);
  VkShaderModuleCreateInfo desc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; desc.codeSize=size; desc.pCode=code.data();
  VkShaderModule module; Check(vkCreateShaderModule(device,&desc,nullptr,&module)); return module;
 }
 Harness(const char *vs,const char *ps) {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="ME2 resolve pixel equivalence"; app.apiVersion=VK_API_VERSION_1_1;
  VkInstanceCreateInfo desc{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; desc.pApplicationInfo=&app; Check(vkCreateInstance(&desc,nullptr,&instance));
  uint32_t count=0; Check(vkEnumeratePhysicalDevices(instance,&count,nullptr)); assert(count);
  std::vector<VkPhysicalDevice> devices(count); Check(vkEnumeratePhysicalDevices(instance,&count,devices.data())); physical=devices[0];
  VkPhysicalDeviceProperties prop; vkGetPhysicalDeviceProperties(physical,&prop); std::printf("Device: %s\n",prop.deviceName);
  vkGetPhysicalDeviceMemoryProperties(physical,&memory);
  vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
  while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family; assert(family<count);
  float priority=1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex=family; q.queueCount=1; q.pQueuePriorities=&priority;
  VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; d.queueCreateInfoCount=1; d.pQueueCreateInfos=&q;
  Check(vkCreateDevice(physical,&d,nullptr,&device)); vkGetDeviceQueue(device,family,0,&queue);
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex=family; Check(vkCreateCommandPool(device,&pool,nullptr,&commands));
  for(unsigned i=0;i<2;++i) {
   VkDescriptorSetLayoutBinding binding{}; binding.descriptorCount=1; binding.descriptorType=i?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
   binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT; VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; l.bindingCount=1; l.pBindings=&binding;
   Check(vkCreateDescriptorSetLayout(device,&l,nullptr,&layouts[i]));
  }
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,32};
  VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; l.setLayoutCount=2; l.pSetLayouts=layouts; l.pushConstantRangeCount=1; l.pPushConstantRanges=&push;
  Check(vkCreatePipelineLayout(device,&l,nullptr,&layout)); vertex=Shader(vs); pixel=Shader(ps);
  VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; s.magFilter=s.minFilter=VK_FILTER_NEAREST; s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
  s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; Check(vkCreateSampler(device,&s,nullptr,&sampler));
 }
 ~Harness() {
  vkDestroySampler(device,sampler,nullptr); vkDestroyShaderModule(device,pixel,nullptr); vkDestroyShaderModule(device,vertex,nullptr);
  vkDestroyPipelineLayout(device,layout,nullptr); for(auto l:layouts) vkDestroyDescriptorSetLayout(device,l,nullptr);
  vkDestroyCommandPool(device,commands,nullptr); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
 }
 VkCommandBuffer Begin() {
  VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; a.commandPool=commands; a.commandBufferCount=1; a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  VkCommandBuffer c; Check(vkAllocateCommandBuffers(device,&a,&c)); VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Check(vkBeginCommandBuffer(c,&b)); return c;
 }
 void Submit(VkCommandBuffer c) {
  Check(vkEndCommandBuffer(c)); VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount=1; s.pCommandBuffers=&c;
  Check(vkQueueSubmit(queue,1,&s,VK_NULL_HANDLE)); Check(vkQueueWaitIdle(queue)); vkFreeCommandBuffers(device,commands,1,&c);
 }
 void Transition(VkCommandBuffer c,Image &image,VkImageLayout next) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image=image.handle; b.oldLayout=image.layout; b.newLayout=next;
  b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  b.srcAccessMask=image.layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b); image.layout=next;
 }
 void MakePipeline(VkFormat format) {
  VkAttachmentDescription a{}; a.format=format; a.samples=VK_SAMPLE_COUNT_1_BIT; a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
  a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE; a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=1; rp.pAttachments=&a; rp.subpassCount=1; rp.pSubpasses=&sub;
  Check(vkCreateRenderPass(device,&rp,nullptr,&pass));
  VkPipelineShaderStageCreateInfo stages[2]{};
  for(auto &s:stages) { s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; s.pName="main"; }
  stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vertex; stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=pixel;
  VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=viewport.scissorCount=1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; msaa.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask=15;
  VkPipelineColorBlendStateCreateInfo color{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; color.attachmentCount=1; color.pAttachments=&blend;
  VkDynamicState dynamics[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamics;
  VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; p.stageCount=2; p.pStages=stages; p.pVertexInputState=&input; p.pInputAssemblyState=&assembly;
  p.pViewportState=&viewport; p.pRasterizationState=&raster; p.pMultisampleState=&msaa; p.pColorBlendState=&color; p.pDynamicState=&dynamic; p.layout=layout; p.renderPass=pass;
  Check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&p,nullptr,&pipeline));
  VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,5},{VK_DESCRIPTOR_TYPE_SAMPLER,1}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=6; dp.poolSizeCount=2; dp.pPoolSizes=sizes;
  Check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors));
  VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ds.descriptorPool=descriptors; ds.descriptorSetCount=1; ds.pSetLayouts=&layouts[1];
  Check(vkAllocateDescriptorSets(device,&ds,&sampler_set)); VkDescriptorImageInfo info{sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=sampler_set; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER; write.pImageInfo=&info;
  vkUpdateDescriptorSets(device,1,&write,0,nullptr);
 }
 Image MakeImage(VkFormat format) {
  Image image; VkImageCreateInfo d{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; d.imageType=VK_IMAGE_TYPE_2D; d.format=format; d.extent={W,H,1};
  d.mipLevels=d.arrayLayers=1; d.samples=VK_SAMPLE_COUNT_1_BIT; d.tiling=VK_IMAGE_TILING_OPTIMAL;
  d.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  Check(vkCreateImage(device,&d,nullptr,&image.handle)); VkMemoryRequirements req; vkGetImageMemoryRequirements(device,image.handle,&req);
  VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; a.allocationSize=req.size; a.memoryTypeIndex=MemoryType(req.memoryTypeBits,0);
  Check(vkAllocateMemory(device,&a,nullptr,&image.memory)); Check(vkBindImageMemory(device,image.handle,image.memory,0));
  VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; v.image=image.handle; v.viewType=VK_IMAGE_VIEW_TYPE_2D; v.format=format; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  Check(vkCreateImageView(device,&v,nullptr,&image.view));
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass=pass; fb.attachmentCount=1; fb.pAttachments=&image.view; fb.width=W; fb.height=H; fb.layers=1;
  Check(vkCreateFramebuffer(device,&fb,nullptr,&image.frame));
  VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ds.descriptorPool=descriptors; ds.descriptorSetCount=1; ds.pSetLayouts=&layouts[0]; Check(vkAllocateDescriptorSets(device,&ds,&image.set));
  VkDescriptorImageInfo info{VK_NULL_HANDLE,image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=image.set; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; write.pImageInfo=&info;
  vkUpdateDescriptorSets(device,1,&write,0,nullptr); return image;
 }
 void Upload(VkCommandBuffer c,Image &image,const Buffer &b) {
  Transition(c,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={W,H,1};
  vkCmdCopyBufferToImage(c,b.handle,image.handle,image.layout,1,&copy);
 }
 void Draw(VkCommandBuffer c,Image &dest,Image &source,VkRect2D rect,int dx,int dy,float factor) {
  Transition(c,source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); Transition(c,dest,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass=pass; begin.framebuffer=dest.frame; begin.renderArea.extent={W,H};
  vkCmdBeginRenderPass(c,&begin,VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
  VkViewport viewport{0,0,float(W),float(H),0,1}; vkCmdSetViewport(c,0,1,&viewport); vkCmdSetScissor(c,0,1,&rect);
  const VkDescriptorSet sets[]={source.set,sampler_set}; vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,2,sets,0,nullptr);
  Push push; push.translate[0]=float(dx); push.translate[1]=float(dy); push.param0=factor;
  vkCmdPushConstants(c,layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,32,&push); vkCmdDraw(c,3,1,0,0); vkCmdEndRenderPass(c);
 }
 void Read(VkCommandBuffer c,Image &image,Buffer &b) {
  Transition(c,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={W,H,1};
  vkCmdCopyImageToBuffer(c,image.handle,image.layout,b.handle,1,&copy);
 }
 void Format(VkFormat format,unsigned channels,unsigned bits,const char *name) {
  VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
  assert(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
  MakePipeline(format); Image source=MakeImage(format),intermediate=MakeImage(format),scratch=MakeImage(format),old=MakeImage(format),fused=MakeImage(format);
  const size_t bytes=size_t(W)*H*channels*(bits/8);
  auto input=MakeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT), sentinel=MakeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  auto a=MakeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT),b=MakeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  // Every 16-bit pattern appears in every channel: signed zero, normals,
  // denormals, infinities and both kinds of NaNs for half-float images.
  for(size_t i=0;i<size_t(W)*H;++i) for(unsigned ch=0;ch<channels;++ch) {
   uint32_t value=(i+ch*16381u)&65535u;
   size_t at=(i*channels+ch)*(bits/8);
   if(bits==16) { uint16_t v=uint16_t(value); std::memcpy(static_cast<uint8_t*>(input.data)+at,&v,2); }
   else static_cast<uint8_t*>(input.data)[at]=uint8_t(value);
  }
  std::memset(sentinel.data,0x35,bytes);
  auto init=Begin(); Upload(init,source,input); Submit(init);
  uint64_t checked=0;
  for(int mode=0;mode<2;++mode) for(int bias=-32;bias<=31;++bias) {
   if(mode && bias!=-32 && bias!=-6 && bias!=-3 && bias!=0 && bias!=7 && bias!=31) continue;
   VkRect2D rect=mode?VkRect2D{{7,11},{W-27,H-31}}:VkRect2D{{0,0},{W,H}};
   const int dx=mode?6:0,dy=mode?-8:0;
   const float scale=std::ldexp(1.f,bias);
   auto c=Begin(); Upload(c,old,sentinel); Upload(c,fused,sentinel);
   // Previous sequence: draw to same-format RT -> raw copy to scratch -> scale draw back.
   Draw(c,intermediate,source,rect,dx,dy,0.f);
   Transition(c,intermediate,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); Transition(c,scratch,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.srcOffset=copy.dstOffset={rect.offset.x,rect.offset.y,0}; copy.extent={rect.extent.width,rect.extent.height,1};
   vkCmdCopyImage(c,intermediate.handle,intermediate.layout,scratch.handle,scratch.layout,1,&copy);
   Draw(c,old,scratch,rect,0,0,scale);
   // New sequence: same shader and coordinates, with scaling in the initial draw.
   Draw(c,fused,source,rect,dx,dy,unscaled_negative_control?1.f:scale); Read(c,old,a); Read(c,fused,b);
   VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr); Submit(c);
   if(std::memcmp(a.data,b.data,bytes)) {
    auto *x=static_cast<uint8_t*>(a.data),*y=static_cast<uint8_t*>(b.data); size_t at=0; while(x[at]==y[at]) ++at;
    std::fprintf(stderr,"DIFFERENCE %s bias %d partial %d at byte %zu: %02x vs %02x\n",name,bias,mode,at,x[at],y[at]); std::exit(1);
   }
   // The red/first stored component of source texel zero is exactly zero.
   // Both paths merely retaining their nonzero sentinel must not pass.
   if(!mode) assert(static_cast<const uint8_t*>(b.data)[0]==0);
   // Outside a partial resolve, both paths must retain the existing destination bytes.
   if(mode) for(uint32_t row=0;row<H;++row) for(uint32_t col=0;col<W;++col) {
    if(col>=7 && col<7+rect.extent.width && row>=11 && row<11+rect.extent.height) continue;
    size_t at=(size_t(row)*W+col)*channels*(bits/8);
    assert(std::memcmp(static_cast<uint8_t*>(b.data)+at,static_cast<uint8_t*>(sentinel.data)+at,channels*(bits/8))==0);
   }
   checked+=bytes; ++cases;
  }
  std::printf("%s: 70 cases, biases -32..31, all input bit patterns, translated partial rectangles: %llu identical bytes\n",name,(unsigned long long)checked); std::fflush(stdout);
  for(auto *image:{&source,&intermediate,&scratch,&old,&fused}) { vkDestroyFramebuffer(device,image->frame,nullptr); vkDestroyImageView(device,image->view,nullptr); vkDestroyImage(device,image->handle,nullptr); vkFreeMemory(device,image->memory,nullptr); }
  Destroy(input); Destroy(sentinel); Destroy(a); Destroy(b);
  vkDestroyDescriptorPool(device,descriptors,nullptr); vkDestroyPipeline(device,pipeline,nullptr); vkDestroyRenderPass(device,pass,nullptr);
 }
};
int main(int argc,char **argv) {
 assert(argc==3 || argc==4); Harness h(argv[1],argv[2]); h.unscaled_negative_control=argc==4;
 h.Format(VK_FORMAT_R8G8B8A8_UNORM,4,8,"RGBA8 UNORM"); h.Format(VK_FORMAT_B8G8R8A8_UNORM,4,8,"BGRA8 UNORM");
 h.Format(VK_FORMAT_R16G16B16A16_UNORM,4,16,"RGBA16 UNORM"); h.Format(VK_FORMAT_R16G16_UNORM,2,16,"RG16 UNORM");
 h.Format(VK_FORMAT_R8G8_UNORM,2,8,"RG8 UNORM"); h.Format(VK_FORMAT_R8_UNORM,1,8,"R8 UNORM"); h.Format(VK_FORMAT_R16_UNORM,1,16,"R16 UNORM");
 h.Format(VK_FORMAT_R16G16B16A16_SFLOAT,4,16,"RGBA16 FLOAT"); h.Format(VK_FORMAT_R16G16_SFLOAT,2,16,"RG16 FLOAT"); h.Format(VK_FORMAT_R16_SFLOAT,1,16,"R16 FLOAT");
 std::printf("PASS: %u actual fragment/render-target cases match byte-for-byte, including alpha\n",h.cases);
}
