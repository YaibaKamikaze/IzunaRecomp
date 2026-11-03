// [IzunaRecomp] A resolve from a 2_10_10_10_FLOAT render target, as the EDRAM
// holds it (native_draw_internal.h: Saturate7e3; shaders/resolve7e3.comp).
//
// The Xbox 360 keeps these targets as unsigned 7e3 colour (0 to 31.875) with a
// 2-bit alpha, and saturates every write and blend to that. The Kasumi
// renderer keeps them as 16-bit float: a blend can go negative or past 31.875.
// NG2's blood darkens the floor with a blend that subtracts; the floor's
// green and blue went negative (-1.3 at worst in the owner's test48 capture)
// where the EDRAM stops at 0, and a post pass dividing by the brightness turned
// them into cyan dots. The targets reach the rest of the frame only through
// resolves, so each resolve of one is saturated in place, as the EDRAM's
// contents would have been.

#include "native/native_draw_internal.h"

namespace ng2native::draw {
namespace {

constexpr uint32_t kResolve7e3Cs[] = {
#include "shaders/resolve7e3_cs.inc"
};
constexpr uint32_t kSetsPerFrame = 64;  // (resolves of 7e3 targets a frame: ~5 in NG2)

struct Saturate {
  bool tried = false, ready = false;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  // A set per use, from the frame half being recorded: a half's sets are
  // free again when it comes round (its frame two back has finished).
  VkDescriptorSet sets[2][kSetsPerFrame] = {};
  uint32_t used[2] = {};
  uint64_t frame[2] = {UINT64_MAX, UINT64_MAX};
} s;

bool Init() {
  s.tried = true;
  const VkDevice device = g.ctx.device;
  VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mci.codeSize = sizeof(kResolve7e3Cs);
  mci.pCode = kResolve7e3Cs;
  VkShaderModule module = VK_NULL_HANDLE;
  if (g.vk.CreateShaderModule(device, &mci, nullptr, &module) != VK_SUCCESS) return false;

  VkDescriptorSetLayoutBinding binding = {};
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dci.bindingCount = 1;
  dci.pBindings = &binding;
  if (g.vk.CreateDescriptorSetLayout(device, &dci, nullptr, &s.set_layout) != VK_SUCCESS) return false;

  const VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
  VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lci.setLayoutCount = 1;
  lci.pSetLayouts = &s.set_layout;
  lci.pushConstantRangeCount = 1;
  lci.pPushConstantRanges = &push;
  if (g.vk.CreatePipelineLayout(device, &lci, nullptr, &s.layout) != VK_SUCCESS) return false;

  VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pci.stage.module = module;
  pci.stage.pName = "main";
  pci.layout = s.layout;
  if (g.vk.CreateComputePipelines(device, g.pipeline_cache, 1, &pci, nullptr, &s.pipeline) != VK_SUCCESS) {
    return false;
  }

  const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSetsPerFrame};
  VkDescriptorPoolCreateInfo poci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poci.maxSets = 2 * kSetsPerFrame;
  poci.poolSizeCount = 1;
  poci.pPoolSizes = &size;
  if (g.vk.CreateDescriptorPool(device, &poci, nullptr, &s.pool) != VK_SUCCESS) return false;
  VkDescriptorSetLayout layouts[2 * kSetsPerFrame];
  for (auto& l : layouts) l = s.set_layout;
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = s.pool;
  ai.descriptorSetCount = 2 * kSetsPerFrame;
  ai.pSetLayouts = layouts;
  if (g.vk.AllocateDescriptorSets(device, &ai, &s.sets[0][0]) != VK_SUCCESS) return false;
  REXLOG_INFO("[native] 7e3 resolves saturated as the EDRAM holds them (0-31.875, 2-bit alpha)");
  return s.ready = true;
}

}  // namespace

bool Saturate7e3(const Image& image, int32_t x, int32_t y, uint32_t w, uint32_t h) {
  if (!image.view || !w || !h || image.format != VK_FORMAT_R16G16B16A16_SFLOAT) return false;
  if (!s.tried) Init();
  if (!s.ready) return false;
  const uint32_t half = g.frame_index & 1;
  if (s.frame[half] != g.frame_number) {
    s.frame[half] = g.frame_number;
    s.used[half] = 0;
  }
  if (s.used[half] == kSetsPerFrame) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("[native] more than {} 7e3 resolves in a frame: the rest are not saturated", kSetsPerFrame);
    }
    return false;
  }
  const VkDescriptorSet set = s.sets[half][s.used[half]++];
  VkDescriptorImageInfo ii = {VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet wds = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wds.dstSet = set;
  wds.descriptorCount = 1;
  wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  wds.pImageInfo = &ii;
  g.vk.UpdateDescriptorSets(g.ctx.device, 1, &wds, 0, nullptr);

  Barrier(g.cmd);  // (the resolve's copy, before the pass reads it)
  g.vk.CmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
  g.vk.CmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &set, 0, nullptr);
  const int32_t rect[4] = {x, y, int32_t(w), int32_t(h)};
  g.vk.CmdPushConstants(g.cmd, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rect), rect);
  g.vk.CmdDispatch(g.cmd, (w + 7) / 8, (h + 7) / 8, 1);
  Barrier(g.cmd);  // (before later draws sample it)
  g.dyn.push_valid = false;  // (push constants of another layout: the draws' must be pushed again)
  return true;
}

}  // namespace ng2native::draw
