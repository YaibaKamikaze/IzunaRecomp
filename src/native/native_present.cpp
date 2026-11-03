// [IzunaRecomp, M4] The native swap (native_present.h).
//
// One draw per guest swap: the native frame (an RGBA8 image, the shared
// queue's earlier work drew it) sampled 1:1 into the presenter's guest output
// (A2B10G10R10, which the presenter scales, letterboxes and shows), each
// channel through the game's 256-entry gamma ramp - what the Xbox's display
// hardware does with the front buffer (the emulated GPU's swap did the same:
// its swap_apply_gamma pass). The guest output is handed back in the layout the
// presenter wants (ui/vulkan/presenter.h, kGuestOutputInternal*).

#include "native/native_present.h"

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/presenter.h>

REXCVAR_DEFINE_STRING(native_antialias, "none", "Native renderer",
                      "[IzunaRecomp] Anti-aliasing of the shown frame: none, fxaa, fxaa_extreme (the hub's "
                      "Anti-aliasing)");

namespace ng2native {
namespace {

constexpr uint32_t kPresentVs[] = {
#include "shaders/present_vs.inc"
};
constexpr uint32_t kPresentFs[] = {
#include "shaders/present_fs.inc"
};

constexpr uint32_t kInFlight = 2;

struct Present {
  bool tried = false, ready = false;
  VkDevice device = VK_NULL_HANDLE;
  const rex::external_frame::VulkanDevice* dev = nullptr;
#define NG2_PRESENT_VK(X)                                                                                       \
  X(CreateCommandPool) X(AllocateCommandBuffers) X(CreateFence) X(WaitForFences) X(ResetFences)                  \
  X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(QueueSubmit)           \
  X(CreateShaderModule) X(CreateDescriptorSetLayout) X(CreatePipelineLayout) X(CreateGraphicsPipelines)          \
  X(CreateDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) X(CreateBuffer)     \
  X(GetBufferMemoryRequirements) X(AllocateMemory) X(BindBufferMemory) X(MapMemory) X(CmdBindPipeline)           \
  X(CmdBindDescriptorSets) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) X(CmdPushConstants)
#define NG2_PRESENT_DECLARE(name) PFN_vk##name name = nullptr;
  NG2_PRESENT_VK(NG2_PRESENT_DECLARE)
  PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
  PFN_vkCmdEndRendering CmdEndRendering = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  VkCommandPool pool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kInFlight> cmd{};
  std::array<VkFence, kInFlight> fence{};
  std::array<VkBuffer, kInFlight> ramp{};
  std::array<uint32_t*, kInFlight> ramp_mapped{};
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  // A descriptor set per (frame image view, in-flight slot): the native
  // renderer shows its frames from two images, made once.
  std::unordered_map<uint64_t, VkDescriptorSet> sets;
  uint32_t next = 0;
  std::mutex mutex;
};

Present& P() {
  static Present* p = new Present;  // (lives as long as the app)
  return *p;
}

uint32_t MemoryType(Present& p, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties props;
  p.GetPhysicalDeviceMemoryProperties(VkPhysicalDevice(p.dev->physical_device), &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  return UINT32_MAX;
}

bool Initialize(Present& p) {
  p.dev = rex::external_frame::GetVulkanDevice();
  if (!p.dev || !p.dev->device || !p.dev->get_instance_proc_addr) return false;
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(p.dev->get_instance_proc_addr);
  const VkInstance instance = VkInstance(p.dev->instance);
  p.device = VkDevice(p.dev->device);
  auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
  p.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
  if (!gdpa || !p.GetPhysicalDeviceMemoryProperties) return false;
  bool ok = true;
#define NG2_PRESENT_LOAD(name) \
  ok = ok && (p.name = reinterpret_cast<PFN_vk##name>(gdpa(p.device, "vk" #name))) != nullptr;
  NG2_PRESENT_VK(NG2_PRESENT_LOAD)
  p.CmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(gdpa(p.device, "vkCmdBeginRendering"));
  if (!p.CmdBeginRendering) p.CmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(gdpa(p.device, "vkCmdBeginRenderingKHR"));
  p.CmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(gdpa(p.device, "vkCmdEndRendering"));
  if (!p.CmdEndRendering) p.CmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(gdpa(p.device, "vkCmdEndRenderingKHR"));
  if (!ok || !p.CmdBeginRendering || !p.CmdEndRendering) return false;

  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = p.dev->queue_family;
  if (p.CreateCommandPool(p.device, &pci, nullptr, &p.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = p.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = kInFlight;
  if (p.AllocateCommandBuffers(p.device, &cai, p.cmd.data()) != VK_SUCCESS) return false;
  for (uint32_t i = 0; i < kInFlight; ++i) {
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (p.CreateFence(p.device, &fci, nullptr, &p.fence[i]) != VK_SUCCESS) return false;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 256 * 4;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (p.CreateBuffer(p.device, &bci, nullptr, &p.ramp[i]) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    p.GetBufferMemoryRequirements(p.device, p.ramp[i], &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = MemoryType(p, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory;
    void* mapped = nullptr;
    if (mai.memoryTypeIndex == UINT32_MAX || p.AllocateMemory(p.device, &mai, nullptr, &memory) != VK_SUCCESS ||
        p.BindBufferMemory(p.device, p.ramp[i], memory, 0) != VK_SUCCESS ||
        p.MapMemory(p.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    p.ramp_mapped[i] = static_cast<uint32_t*>(mapped);
  }
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;  // (FXAA's taps; without it the pass reads texels exactly)
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (p.CreateSampler(p.device, &sci, nullptr, &p.sampler) != VK_SUCCESS) return false;
  VkDescriptorSetLayoutBinding bindings[2] = {};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[0].pImmutableSamplers = &p.sampler;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dlci.bindingCount = 2;
  dlci.pBindings = bindings;
  if (p.CreateDescriptorSetLayout(p.device, &dlci, nullptr, &p.set_layout) != VK_SUCCESS) return false;
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &p.set_layout;
  const VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};  // (the anti-aliasing mode)
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &push;
  if (p.CreatePipelineLayout(p.device, &plci, nullptr, &p.layout) != VK_SUCCESS) return false;
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 16;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = sizes;
  if (p.CreateDescriptorPool(p.device, &dpci, nullptr, &p.descriptor_pool) != VK_SUCCESS) return false;

  VkShaderModule vs, fs;
  VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = sizeof(kPresentVs);
  smci.pCode = kPresentVs;
  if (p.CreateShaderModule(p.device, &smci, nullptr, &vs) != VK_SUCCESS) return false;
  smci.codeSize = sizeof(kPresentFs);
  smci.pCode = kPresentFs;
  if (p.CreateShaderModule(p.device, &smci, nullptr, &fs) != VK_SUCCESS) return false;
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {};
  cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                       VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  const VkFormat format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.pNext = &ri;
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &cb;
  gpci.pDynamicState = &ds;
  gpci.layout = p.layout;
  if (p.CreateGraphicsPipelines(p.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &p.pipeline) != VK_SUCCESS) {
    return false;
  }
  REXLOG_INFO("[izuna] present pass ready (the game's gamma ramp, {} in flight)", kInFlight);
  return true;
}

VkDescriptorSet DescriptorSet(Present& p, VkImageView frame, uint32_t slot) {
  const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(frame)) ^ (uint64_t(slot) << 62);
  if (auto it = p.sets.find(key); it != p.sets.end()) return it->second;
  if (p.sets.size() >= 16) return VK_NULL_HANDLE;  // (the frames' images are made once: never this many)
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = p.descriptor_pool;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &p.set_layout;
  VkDescriptorSet set;
  if (p.AllocateDescriptorSets(p.device, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkDescriptorImageInfo ii = {VK_NULL_HANDLE, frame, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo bi = {p.ramp[slot], 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  w[0].dstSet = set;
  w[0].dstBinding = 0;
  w[0].descriptorCount = 1;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w[0].pImageInfo = &ii;
  w[1].dstSet = set;
  w[1].dstBinding = 1;
  w[1].descriptorCount = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[1].pBufferInfo = &bi;
  p.UpdateDescriptorSets(p.device, 2, w, 0, nullptr);
  p.sets.emplace(key, set);
  return set;
}

void ImageBarrier(Present& p, VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags src_stage,
                  VkPipelineStageFlags dst_stage, VkAccessFlags src_access, VkAccessFlags dst_access,
                  VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  p.CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

bool PresentNativeFrame(rex::ui::Presenter& presenter, const uint32_t* gamma_ramp) {
  Present& p = P();
  std::lock_guard<std::mutex> lock(p.mutex);
  if (!p.tried) {
    p.tried = true;
    p.ready = Initialize(p);
    if (!p.ready) REXLOG_ERROR("[izuna] the present pass could not be made: nothing will be shown");
  }
  if (!p.ready) return false;
  rex::external_frame::Frame frame;
  if (!rex::external_frame::GetFrame(frame) || !frame.vk_image_view || !frame.width || !frame.height) return false;
  return presenter.RefreshGuestOutput(
      frame.width, frame.height, frame.width, frame.height, [&](rex::ui::Presenter::GuestOutputRefreshContext& ctx) {
        auto& out = static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(ctx);
        ctx.SetIs8bpc(true);
        const uint32_t slot = p.next;
        p.next = (p.next + 1) % kInFlight;
        p.WaitForFences(p.device, 1, &p.fence[slot], VK_TRUE, UINT64_MAX);
        std::memcpy(p.ramp_mapped[slot], gamma_ramp, 256 * 4);
        const VkDescriptorSet set = DescriptorSet(p, VkImageView(frame.vk_image_view), slot);
        if (!set) return false;
        VkCommandBuffer cmd = p.cmd[slot];
        p.ResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        p.BeginCommandBuffer(cmd, &cbi);
        // (the frame: drawn by earlier work on this queue, kept in SHADER_READ_ONLY_OPTIMAL)
        ImageBarrier(p, cmd, VkImage(frame.vk_image),
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        using rex::ui::vulkan::VulkanPresenter;
        const bool written = out.image_ever_written_previously();
        ImageBarrier(p, cmd, out.image(),
                     written ? VkPipelineStageFlags(VulkanPresenter::kGuestOutputInternalStageMask)
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     written ? VulkanPresenter::kGuestOutputInternalAccessMask : 0,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = out.image_view();
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {frame.width, frame.height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        p.CmdBeginRendering(cmd, &ri);
        const VkViewport viewport = {0.0f, 0.0f, float(frame.width), float(frame.height), 0.0f, 1.0f};
        const VkRect2D scissor = {{0, 0}, {frame.width, frame.height}};
        p.CmdSetViewport(cmd, 0, 1, &viewport);
        p.CmdSetScissor(cmd, 0, 1, &scissor);
        p.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
        p.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &set, 0, nullptr);
        // (read at every present: the overlay changes it while the game runs)
        const std::string aa = REXCVAR_GET(native_antialias);
        const uint32_t mode = aa == "fxaa_extreme" ? 2u : aa == "fxaa" ? 1u : 0u;
        p.CmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &mode);
        p.CmdDraw(cmd, 3, 1, 0, 0);
        p.CmdEndRendering(cmd);
        ImageBarrier(p, cmd, out.image(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VulkanPresenter::kGuestOutputInternalStageMask, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VulkanPresenter::kGuestOutputInternalAccessMask, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VulkanPresenter::kGuestOutputInternalLayout);
        p.EndCommandBuffer(cmd);
        p.ResetFences(p.device, 1, &p.fence[slot]);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        std::lock_guard<std::recursive_mutex> queue_lock(*p.dev->queue_mutex);
        return p.QueueSubmit(VkQueue(p.dev->queue), 1, &si, p.fence[slot]) == VK_SUCCESS;
      });
}

}  // namespace ng2native
