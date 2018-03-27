#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/include/rapidjson/document.h"
#include "rapidjson/include/rapidjson/prettywriter.h"
using namespace rapidjson;
#include "b64.h"

#include "vulkan_pipeline_cache.hpp"
#include <stdexcept>
#include <algorithm>
#include <string.h>

using namespace std;

namespace VPC
{
namespace Hashing
{
Hash compute_hash_descriptor_set_layout(const StateRecorder &recorder, const VkDescriptorSetLayoutCreateInfo &layout)
{
	Hasher h;

	h.u32(layout.bindingCount);
	h.u32(layout.flags);
	for (uint32_t i = 0; i < layout.bindingCount; i++)
	{
		auto &binding = layout.pBindings[i];
		h.u32(binding.binding);
		h.u32(binding.descriptorCount);
		h.u32(binding.descriptorType);
		h.u32(binding.stageFlags);

		if (binding.pImmutableSamplers &&
			(binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
		    binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER))
		{
			for (uint32_t j = 0; j < binding.descriptorCount; j++)
				h.u64(recorder.get_hash_for_sampler(binding.pImmutableSamplers[j]));
		}
	}

	return h.get();
}

Hash compute_hash_pipeline_layout(const StateRecorder &recorder, const VkPipelineLayoutCreateInfo &layout)
{
	Hasher h;

	h.u32(layout.setLayoutCount);
	for (uint32_t i = 0; i < layout.setLayoutCount; i++)
	{
		if (layout.pSetLayouts[i])
			h.u64(recorder.get_hash_for_descriptor_set_layout(layout.pSetLayouts[i]));
		else
			h.u32(0);
	}

	h.u32(layout.pushConstantRangeCount);
	for (uint32_t i = 0; i < layout.pushConstantRangeCount; i++)
	{
		auto &push = layout.pPushConstantRanges[i];
		h.u32(push.stageFlags);
		h.u32(push.size);
		h.u32(push.offset);
	}

	h.u32(layout.flags);

	return h.get();
}

Hash compute_hash_shader_module(const StateRecorder &, const VkShaderModuleCreateInfo &create_info)
{
	Hasher h;
	h.data(create_info.pCode, create_info.codeSize);
	h.u32(create_info.flags);
	return h.get();
}

static void hash_specialization_info(Hasher &h, const VkSpecializationInfo &spec)
{
	h.data(static_cast<const uint8_t *>(spec.pData), spec.dataSize);
	h.u32(spec.dataSize);
	h.u32(spec.mapEntryCount);
	for (uint32_t i = 0; i < spec.mapEntryCount; i++)
	{
		h.u32(spec.pMapEntries[i].offset);
		h.u32(spec.pMapEntries[i].size);
		h.u32(spec.pMapEntries[i].constantID);
	}
}

Hash compute_hash_graphics_pipeline(const StateRecorder &recorder, const VkGraphicsPipelineCreateInfo &create_info)
{
	Hasher h;

	h.u32(create_info.flags);

	if (create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		h.u64(recorder.get_hash_for_graphics_pipeline_handle(create_info.basePipelineHandle));
		h.s32(create_info.basePipelineIndex);
	}

	h.u64(recorder.get_hash_for_pipeline_layout(create_info.layout));
	h.u64(recorder.get_hash_for_render_pass(create_info.renderPass));
	h.u32(create_info.subpass);
	h.u32(create_info.stageCount);

	bool dynamic_stencil_compare = false;
	bool dynamic_stencil_reference = false;
	bool dynamic_stencil_write_mask = false;
	bool dynamic_depth_bounds = false;
	bool dynamic_depth_bias = false;
	bool dynamic_line_width = false;
	bool dynamic_blend_constants = false;
	bool dynamic_scissor = false;
	bool dynamic_viewport = false;
	if (create_info.pDynamicState)
	{
		auto &state = *create_info.pDynamicState;
		h.u32(state.dynamicStateCount);
		h.u32(state.flags);
		for (uint32_t i = 0; i < state.dynamicStateCount; i++)
		{
			h.u32(state.pDynamicStates[i]);
			switch (state.pDynamicStates[i])
			{
			case VK_DYNAMIC_STATE_DEPTH_BIAS:
				dynamic_depth_bias = true;
				break;
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
				dynamic_depth_bounds = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
				dynamic_stencil_write_mask = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
				dynamic_stencil_reference = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
				dynamic_stencil_compare = true;
				break;
			case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
				dynamic_blend_constants = true;
				break;
			case VK_DYNAMIC_STATE_SCISSOR:
				dynamic_scissor = true;
				break;
			case VK_DYNAMIC_STATE_VIEWPORT:
				dynamic_viewport = true;
				break;
			case VK_DYNAMIC_STATE_LINE_WIDTH:
				dynamic_line_width = true;
				break;
			default:
				break;
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pDepthStencilState)
	{
		auto &ds = *create_info.pDepthStencilState;
		h.u32(ds.flags);
		h.u32(ds.depthBoundsTestEnable);
		h.u32(ds.depthCompareOp);
		h.u32(ds.depthTestEnable);
		h.u32(ds.depthWriteEnable);
		h.u32(ds.front.compareOp);
		h.u32(ds.front.depthFailOp);
		h.u32(ds.front.failOp);
		h.u32(ds.front.passOp);
		h.u32(ds.back.compareOp);
		h.u32(ds.back.depthFailOp);
		h.u32(ds.back.failOp);
		h.u32(ds.back.passOp);
		h.u32(ds.stencilTestEnable);

		if (!dynamic_depth_bounds && ds.depthBoundsTestEnable)
		{
			h.f32(ds.minDepthBounds);
			h.f32(ds.maxDepthBounds);
		}

		if (ds.stencilTestEnable)
		{
			if (!dynamic_stencil_compare)
			{
				h.u32(ds.front.compareMask);
				h.u32(ds.back.compareMask);
			}

			if (!dynamic_stencil_reference)
			{
				h.u32(ds.front.reference);
				h.u32(ds.back.reference);
			}

			if (!dynamic_stencil_write_mask)
			{
				h.u32(ds.front.writeMask);
				h.u32(ds.back.writeMask);
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pInputAssemblyState)
	{
		auto &ia = *create_info.pInputAssemblyState;
		h.u32(ia.flags);
		h.u32(ia.primitiveRestartEnable);
		h.u32(ia.topology);
	}
	else
		h.u32(0);

	if (create_info.pRasterizationState)
	{
		auto &rs = *create_info.pRasterizationState;
		h.u32(rs.flags);
		h.u32(rs.cullMode);
		h.u32(rs.depthClampEnable);
		h.u32(rs.frontFace);
		h.u32(rs.rasterizerDiscardEnable);
		h.u32(rs.polygonMode);
		h.u32(rs.depthBiasEnable);

		if (rs.depthBiasEnable && !dynamic_depth_bias)
		{
			h.f32(rs.depthBiasClamp);
			h.f32(rs.depthBiasSlopeFactor);
			h.f32(rs.depthBiasConstantFactor);
		}

		if (!dynamic_line_width)
			h.f32(rs.lineWidth);
	}
	else
		h.u32(0);

	if (create_info.pMultisampleState)
	{
		auto &ms = *create_info.pMultisampleState;
		h.u32(ms.flags);
		h.u32(ms.alphaToCoverageEnable);
		h.u32(ms.alphaToOneEnable);
		h.f32(ms.minSampleShading);
		h.u32(ms.rasterizationSamples);
		h.u32(ms.sampleShadingEnable);
		if (ms.pSampleMask)
		{
			uint32_t elems = (ms.rasterizationSamples + 31) / 32;
			for (uint32_t i = 0; i < elems; i++)
				h.u32(ms.pSampleMask[i]);
		}
		else
			h.u32(0);
	}

	if (create_info.pViewportState)
	{
		auto &vp = *create_info.pViewportState;
		h.u32(vp.flags);
		h.u32(vp.scissorCount);
		h.u32(vp.viewportCount);
		if (!dynamic_scissor)
		{
			for (uint32_t i = 0; i < vp.scissorCount; i++)
			{
				h.s32(vp.pScissors[i].offset.x);
				h.s32(vp.pScissors[i].offset.y);
				h.u32(vp.pScissors[i].extent.width);
				h.u32(vp.pScissors[i].extent.height);
			}
		}

		if (!dynamic_viewport)
		{
			for (uint32_t i = 0; i < vp.viewportCount; i++)
			{
				h.f32(vp.pViewports[i].x);
				h.f32(vp.pViewports[i].y);
				h.f32(vp.pViewports[i].width);
				h.f32(vp.pViewports[i].height);
				h.f32(vp.pViewports[i].minDepth);
				h.f32(vp.pViewports[i].maxDepth);
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pVertexInputState)
	{
		auto &vi = *create_info.pVertexInputState;
		h.u32(vi.flags);
		h.u32(vi.vertexAttributeDescriptionCount);
		h.u32(vi.vertexBindingDescriptionCount);

		for (uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; i++)
		{
			h.u32(vi.pVertexAttributeDescriptions[i].offset);
			h.u32(vi.pVertexAttributeDescriptions[i].binding);
			h.u32(vi.pVertexAttributeDescriptions[i].format);
			h.u32(vi.pVertexAttributeDescriptions[i].location);
		}

		for (uint32_t i = 0; i < vi.vertexBindingDescriptionCount; i++)
		{
			h.u32(vi.pVertexBindingDescriptions[i].binding);
			h.u32(vi.pVertexBindingDescriptions[i].inputRate);
			h.u32(vi.pVertexBindingDescriptions[i].stride);
		}
	}
	else
		h.u32(0);

	if (create_info.pColorBlendState)
	{
		auto &b = *create_info.pColorBlendState;
		h.u32(b.flags);
		h.u32(b.attachmentCount);
		h.u32(b.logicOpEnable);
		h.u32(b.logicOp);

		bool need_blend_constants = false;

		for (uint32_t i = 0; i < b.attachmentCount; i++)
		{
			h.u32(b.pAttachments[i].blendEnable);
			if (b.pAttachments[i].blendEnable)
			{
				h.u32(b.pAttachments[i].colorWriteMask);
				h.u32(b.pAttachments[i].alphaBlendOp);
				h.u32(b.pAttachments[i].colorBlendOp);
				h.u32(b.pAttachments[i].dstAlphaBlendFactor);
				h.u32(b.pAttachments[i].srcAlphaBlendFactor);
				h.u32(b.pAttachments[i].dstColorBlendFactor);
				h.u32(b.pAttachments[i].srcColorBlendFactor);

				if (b.pAttachments[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				    b.pAttachments[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
				    b.pAttachments[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				    b.pAttachments[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
					b.pAttachments[i].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
					b.pAttachments[i].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
					b.pAttachments[i].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
					b.pAttachments[i].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR)
				{
					need_blend_constants = true;
				}
			}
			else
				h.u32(0);
		}

		if (need_blend_constants && !dynamic_blend_constants)
			for (auto &blend_const : b.blendConstants)
				h.f32(blend_const);
	}
	else
		h.u32(0);

	if (create_info.pTessellationState)
	{
		auto &tess = *create_info.pTessellationState;
		h.u32(tess.flags);
		h.u32(tess.patchControlPoints);
	}
	else
		h.u32(0);

	for (uint32_t i = 0; i < create_info.stageCount; i++)
	{
		auto &stage = create_info.pStages[i];
		h.u32(stage.flags);
		h.string(stage.pName);
		h.u32(stage.stage);
		h.u64(recorder.get_hash_for_shader_module(stage.module));
		if (stage.pSpecializationInfo)
		{
			hash_specialization_info(h, *stage.pSpecializationInfo);

		}
		else
			h.u32(0);
	}

	return h.get();
}

Hash compute_hash_compute_pipeline(const StateRecorder &recorder, const VkComputePipelineCreateInfo &create_info)
{
	Hasher h;

	h.u64(recorder.get_hash_for_pipeline_layout(create_info.layout));
	h.u32(create_info.flags);

	if (create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		h.u64(recorder.get_hash_for_compute_pipeline_handle(create_info.basePipelineHandle));
		h.s32(create_info.basePipelineIndex);
	}
	else
		h.u32(0);

	h.u64(recorder.get_hash_for_shader_module(create_info.stage.module));
	h.string(create_info.stage.pName);
	h.u32(create_info.stage.flags);
	h.u32(create_info.stage.stage);

	if (create_info.stage.pSpecializationInfo)
		hash_specialization_info(h, *create_info.stage.pSpecializationInfo);
	else
		h.u32(0);

	return h.get();
}

static void hash_attachment(Hasher &h, const VkAttachmentDescription &att)
{
	h.u32(att.flags);
	h.u32(att.initialLayout);
	h.u32(att.finalLayout);
	h.u32(att.format);
	h.u32(att.loadOp);
	h.u32(att.storeOp);
	h.u32(att.stencilLoadOp);
	h.u32(att.stencilStoreOp);
	h.u32(att.samples);
}

static void hash_dependency(Hasher &h, const VkSubpassDependency &dep)
{
	h.u32(dep.dependencyFlags);
	h.u32(dep.dstAccessMask);
	h.u32(dep.srcAccessMask);
	h.u32(dep.srcSubpass);
	h.u32(dep.dstSubpass);
	h.u32(dep.srcStageMask);
	h.u32(dep.dstStageMask);
}

static void hash_subpass(Hasher &h, const VkSubpassDescription &subpass)
{
	h.u32(subpass.flags);
	h.u32(subpass.colorAttachmentCount);
	h.u32(subpass.inputAttachmentCount);
	h.u32(subpass.preserveAttachmentCount);
	h.u32(subpass.pipelineBindPoint);

	for (uint32_t i = 0; i < subpass.preserveAttachmentCount; i++)
		h.u32(subpass.pPreserveAttachments[i]);

	for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
	{
		h.u32(subpass.pColorAttachments[i].attachment);
		h.u32(subpass.pColorAttachments[i].layout);
	}

	for (uint32_t i = 0; i < subpass.inputAttachmentCount; i++)
	{
		h.u32(subpass.pInputAttachments[i].attachment);
		h.u32(subpass.pInputAttachments[i].layout);
	}

	if (subpass.pResolveAttachments)
	{
		for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
		{
			h.u32(subpass.pResolveAttachments[i].attachment);
			h.u32(subpass.pResolveAttachments[i].layout);
		}
	}

	if (subpass.pDepthStencilAttachment)
	{
		h.u32(subpass.pDepthStencilAttachment->attachment);
		h.u32(subpass.pDepthStencilAttachment->layout);
	}
	else
		h.u32(0);
}

Hash compute_hash_render_pass(const StateRecorder &, const VkRenderPassCreateInfo &create_info)
{
	Hasher h;

	h.u32(create_info.attachmentCount);
	h.u32(create_info.dependencyCount);
	h.u32(create_info.subpassCount);

	for (uint32_t i = 0; i < create_info.attachmentCount; i++)
	{
		auto &att = create_info.pAttachments[i];
		hash_attachment(h, att);
	}

	for (uint32_t i = 0; i < create_info.dependencyCount; i++)
	{
		auto &dep = create_info.pDependencies[i];
		hash_dependency(h, dep);
	}

	for (uint32_t i = 0; i < create_info.subpassCount; i++)
	{
		auto &subpass = create_info.pSubpasses[i];
		hash_subpass(h, subpass);
	}

	return h.get();
}
}

static uint32_t *decode_base64(ScratchAllocator &allocator, const char *data, size_t size)
{
	unsigned char *buffer = b64_decode(data, strlen(data));
	auto *ret = static_cast<uint32_t *>(allocator.allocate_raw(size, 4));
	memcpy(ret, buffer, size);
	free(buffer);
	return ret;
}

VkSampler *StateReplayer::parse_immutable_samplers(const Value &samplers)
{
	auto *samps = allocator.allocate_n<VkSampler>(samplers.Size());
	auto *ret = samps;
	for (auto itr = samplers.Begin(); itr != samplers.End(); ++itr, samps++)
	{
		auto index = itr->GetUint64();
		if (index > replayed_samplers.size())
			throw logic_error("Sampler index out of range.");
		else if (index > 0)
			*samps = replayed_samplers[index - 1];
		else
			*samps = VK_NULL_HANDLE;
	}

	return ret;
}

VkDescriptorSetLayoutBinding *StateReplayer::parse_descriptor_set_bindings(const Value &bindings)
{
	auto *set_bindings = allocator.allocate_n_cleared<VkDescriptorSetLayoutBinding>(bindings.Size());
	auto *ret = set_bindings;
	for (auto itr = bindings.Begin(); itr != bindings.End(); ++itr, set_bindings++)
	{
		auto &b = *itr;
		set_bindings->binding = b["binding"].GetUint();
		set_bindings->descriptorCount = b["descriptorCount"].GetUint();
		set_bindings->descriptorType = static_cast<VkDescriptorType>(b["descriptorType"].GetUint());
		set_bindings->stageFlags = b["stageFlags"].GetUint();
		if (b.HasMember("immutableSamplers"))
			set_bindings->pImmutableSamplers = parse_immutable_samplers(b["immutableSamplers"]);
	}
	return ret;
}

VkPushConstantRange *StateReplayer::parse_push_constant_ranges(const Value &ranges)
{
	auto *infos = allocator.allocate_n_cleared<VkPushConstantRange>(ranges.Size());
	auto *ret = infos;

	for (auto itr = ranges.Begin(); itr != ranges.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->stageFlags = obj["stageFlags"].GetUint();
		infos->offset = obj["offset"].GetUint();
		infos->size = obj["size"].GetUint();
	}

	return ret;
}

VkDescriptorSetLayout *StateReplayer::parse_set_layouts(const Value &layouts)
{
	auto *infos = allocator.allocate_n_cleared<VkDescriptorSetLayout>(layouts.Size());
	auto *ret = infos;

	for (auto itr = layouts.Begin(); itr != layouts.End(); ++itr, infos++)
	{
		auto index = itr->GetUint();
		if (index > replayed_descriptor_set_layouts.size())
			throw logic_error("Descriptor set index out of range.");
		else if (index > 0)
			*infos = replayed_descriptor_set_layouts[index - 1];
		else
			*infos = VK_NULL_HANDLE;
	}

	return ret;
}

void StateReplayer::parse_shader_modules(StateCreatorInterface &iface, const Value &modules)
{
	iface.set_num_shader_modules(modules.Size());
	replayed_shader_modules.resize(modules.Size());
	auto *infos = allocator.allocate_n_cleared<VkShaderModuleCreateInfo>(modules.Size());

	unsigned index = 0;
	for (auto itr = modules.Begin(); itr != modules.End(); ++itr, index++)
	{
		auto &obj = *itr;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.flags = obj["flags"].GetUint();
		info.codeSize = obj["codeSize"].GetUint64();
		info.pCode = decode_base64(allocator, obj["code"].GetString(), info.codeSize);
		iface.enqueue_create_shader_module(obj["hash"].GetUint64(), index, &info, &replayed_shader_modules[index]);
	}
	iface.wait_enqueue();
}

void StateReplayer::parse_pipeline_layouts(StateCreatorInterface &iface, const Value &layouts)
{
	iface.set_num_pipeline_layouts(layouts.Size());
	replayed_pipeline_layouts.resize(layouts.Size());
	auto *infos = allocator.allocate_n_cleared<VkPipelineLayoutCreateInfo>(layouts.Size());

	unsigned index = 0;
	for (auto itr = layouts.Begin(); itr != layouts.End(); ++itr, index++)
	{
		auto &obj = *itr;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

		info.flags = obj["flags"].GetUint();

		if (obj.HasMember("pushConstantRanges"))
		{
			info.pushConstantRangeCount = obj["pushConstantRanges"].Size();
			info.pPushConstantRanges = parse_push_constant_ranges(obj["pushConstantRanges"]);
		}

		if (obj.HasMember("setLayouts"))
		{
			info.setLayoutCount = obj["setLayouts"].Size();
			info.pSetLayouts = parse_set_layouts(obj["setLayouts"]);
		}

		iface.enqueue_create_pipeline_layout(obj["hash"].GetUint64(), index, &info, &replayed_pipeline_layouts[index]);
	}
	iface.wait_enqueue();
}

void StateReplayer::parse_descriptor_set_layouts(StateCreatorInterface &iface, const Value &layouts)
{
	iface.set_num_descriptor_set_layouts(layouts.Size());
	replayed_descriptor_set_layouts.resize(layouts.Size());
	auto *infos = allocator.allocate_n_cleared<VkDescriptorSetLayoutCreateInfo>(layouts.Size());

	unsigned index = 0;
	for (auto itr = layouts.Begin(); itr != layouts.End(); ++itr, index++)
	{
		auto &obj = *itr;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

		info.flags = obj["flags"].GetUint();
		if (obj.HasMember("bindings"))
		{
			auto &bindings = obj["bindings"];
			info.bindingCount = bindings.Size();
			info.pBindings = parse_descriptor_set_bindings(bindings);
		}

		iface.enqueue_create_descriptor_set_layout(obj["hash"].GetUint64(), index, &info, &replayed_descriptor_set_layouts[index]);
	}
	iface.wait_enqueue();
}

void StateReplayer::parse_samplers(StateCreatorInterface &iface, const Value &samplers)
{
	iface.set_num_samplers(samplers.Size());
	replayed_samplers.resize(samplers.Size());
	auto *infos = allocator.allocate_n_cleared<VkSamplerCreateInfo>(samplers.Size());

	unsigned index = 0;
	for (auto itr = samplers.Begin(); itr != samplers.End(); ++itr, index++)
	{
		auto &obj = *itr;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

		info.addressModeU = static_cast<VkSamplerAddressMode>(obj["addressModeU"].GetUint());
		info.addressModeV = static_cast<VkSamplerAddressMode>(obj["addressModeV"].GetUint());
		info.addressModeW = static_cast<VkSamplerAddressMode>(obj["addressModeW"].GetUint());
		info.anisotropyEnable = obj["anisotropyEnable"].GetUint();
		info.borderColor = static_cast<VkBorderColor>(obj["borderColor"].GetUint());
		info.compareEnable = obj["compareEnable"].GetUint();
		info.compareOp = static_cast<VkCompareOp>(obj["compareOp"].GetUint());
		info.flags = obj["flags"].GetUint();
		info.magFilter = static_cast<VkFilter>(obj["magFilter"].GetUint());
		info.minFilter = static_cast<VkFilter>(obj["minFilter"].GetUint());
		info.maxAnisotropy = obj["maxAnisotropy"].GetFloat();
		info.mipmapMode = static_cast<VkSamplerMipmapMode>(obj["mipmapMode"].GetUint());
		info.maxLod = obj["maxLod"].GetFloat();
		info.minLod = obj["minLod"].GetFloat();
		info.mipLodBias = obj["mipLodBias"].GetFloat();

		iface.enqueue_create_sampler(obj["hash"].GetUint64(), index, &info, &replayed_samplers[index]);
	}
	iface.wait_enqueue();
}

VkAttachmentDescription *StateReplayer::parse_render_pass_attachments(const Value &attachments)
{
	auto *infos = allocator.allocate_n_cleared<VkAttachmentDescription>(attachments.Size());
	auto *ret = infos;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->flags = obj["flags"].GetUint();
		infos->finalLayout = static_cast<VkImageLayout>(obj["finalLayout"].GetUint());
		infos->initialLayout = static_cast<VkImageLayout>(obj["initialLayout"].GetUint());
		infos->format = static_cast<VkFormat>(obj["format"].GetUint());
		infos->loadOp = static_cast<VkAttachmentLoadOp>(obj["loadOp"].GetUint());
		infos->storeOp = static_cast<VkAttachmentStoreOp>(obj["storeOp"].GetUint());
		infos->stencilLoadOp = static_cast<VkAttachmentLoadOp>(obj["stencilLoadOp"].GetUint());
		infos->stencilStoreOp = static_cast<VkAttachmentStoreOp>(obj["stencilStoreOp"].GetUint());
		infos->samples = static_cast<VkSampleCountFlagBits>(obj["samples"].GetUint());
	}

	return ret;
}

VkSubpassDependency *StateReplayer::parse_render_pass_dependencies(const Value &dependencies)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDependency>(dependencies.Size());
	auto *ret = infos;

	for (auto itr = dependencies.Begin(); itr != dependencies.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->dependencyFlags = obj["dependencyFlags"].GetUint();
		infos->dstAccessMask = obj["dstAccessMask"].GetUint();
		infos->srcAccessMask = obj["srcAccessMask"].GetUint();
		infos->dstStageMask = obj["dstStageMask"].GetUint();
		infos->srcStageMask = obj["srcStageMask"].GetUint();
		infos->srcSubpass = obj["srcSubpass"].GetUint();
		infos->dstSubpass = obj["dstSubpass"].GetUint();
	}

	return ret;
}

VkAttachmentReference *StateReplayer::parse_attachment(const Value &value)
{
	auto *ret = allocator.allocate_cleared<VkAttachmentReference>();
	ret->attachment = value["attachment"].GetUint();
	ret->layout = static_cast<VkImageLayout>(value["layout"].GetUint());
	return ret;
}

VkAttachmentReference *StateReplayer::parse_attachments(const Value &attachments)
{
	auto *refs = allocator.allocate_cleared<VkAttachmentReference>();
	auto *ret = refs;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, refs++)
	{
		auto &value = *itr;
		refs->attachment = value["attachment"].GetUint();
		refs->layout = static_cast<VkImageLayout>(value["layout"].GetUint());
	}
	return ret;
}

VkSubpassDescription *StateReplayer::parse_render_pass_subpasses(const Value &subpasses)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDescription>(subpasses.Size());
	auto *ret = infos;

	for (auto itr = subpasses.Begin(); itr != subpasses.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->flags = obj["flags"].GetUint();

		if (obj.HasMember("depthStencilAttachment"))
			infos->pDepthStencilAttachment = parse_attachment(obj["depthStencilAttachment"]);
		if (obj.HasMember("resolveAttachments"))
			infos->pResolveAttachments = parse_attachments(obj["resolveAttachments"]);
		if (obj.HasMember("inputAttachments"))
			infos->pInputAttachments = parse_attachments(obj["inputAttachments"]);
	}

	return ret;
}

void StateReplayer::parse_render_passes(StateCreatorInterface &iface, const Value &passes)
{
	iface.set_num_render_passes(passes.Size());
	replayed_render_passes.resize(passes.Size());
	auto *infos = allocator.allocate_n_cleared<VkRenderPassCreateInfo>(passes.Size());

	unsigned index = 0;
	for (auto itr = passes.Begin(); itr != passes.End(); ++itr, index++)
	{
		auto &obj = *itr;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;

		info.flags = obj["flags"].GetUint();

		if (obj.HasMember("attachments"))
		{
			info.attachmentCount = obj["attachments"].Size();
			info.pAttachments = parse_render_pass_attachments(obj["attachments"]);
		}

		if (obj.HasMember("dependencies"))
		{
			info.dependencyCount = obj["dependencies"].Size();
			info.pDependencies = parse_render_pass_dependencies(obj["dependencies"]);
		}

		if (obj.HasMember("subpasses"))
		{
			info.subpassCount = obj["subpasses"].Size();
			info.pSubpasses = parse_render_pass_subpasses(obj["subpasses"]);
		}

		iface.enqueue_create_render_pass(obj["hash"].GetUint64(), index, &info, &replayed_render_passes[index]);
	}

	iface.wait_enqueue();
}

bool StateReplayer::parse(StateCreatorInterface &iface, const char *str, size_t length)
{
	Document doc;
	doc.Parse(str, length);
	if (doc.HasParseError())
		return false;

	try
	{
		if (doc.HasMember("shaderModules"))
			parse_shader_modules(iface, doc["shaderModules"]);
		else
			iface.set_num_shader_modules(0);

		if (doc.HasMember("samplers"))
			parse_samplers(iface, doc["samplers"]);
		else
			iface.set_num_samplers(0);

		if (doc.HasMember("descriptorSetLayouts"))
			parse_descriptor_set_layouts(iface, doc["descriptorSetLayouts"]);
		else
			iface.set_num_descriptor_set_layouts(0);

		if (doc.HasMember("pipelineLayouts"))
			parse_pipeline_layouts(iface, doc["pipelineLayouts"]);
		else
			iface.set_num_pipeline_layouts(0);

		if (doc.HasMember("renderPasses"))
			parse_render_passes(iface, doc["renderPasses"]);
		else
			iface.set_num_render_passes(0);
	}
	catch (const exception &e)
	{
		return false;
	}

	return true;
}

template <typename T>
T *StateReplayer::copy(const T *src, size_t count)
{
	auto *new_data = allocator.allocate_n<T>(count);
	if (new_data)
		std::copy(src, src + count, new_data);
	return new_data;
}

template <typename T>
T *StateRecorder::copy(const T *src, size_t count)
{
	auto *new_data = allocator.allocate_n<T>(count);
	if (new_data)
		std::copy(src, src + count, new_data);
	return new_data;
}

ScratchAllocator::Block::Block(size_t size)
{
	blob.reset(new uint8_t[size]);
	this->size = size;
}

void ScratchAllocator::add_block(size_t minimum_size)
{
	if (minimum_size < 64 * 1024)
		minimum_size = 64 * 1024;
	blocks.emplace_back(minimum_size);
}

void *ScratchAllocator::allocate_raw_cleared(size_t size, size_t alignment)
{
	void *ret = allocate_raw(size, alignment);
	if (ret)
		memset(ret, 0, size);
	return ret;
}

void *ScratchAllocator::allocate_raw(size_t size, size_t alignment)
{
	if (blocks.empty())
		add_block(size + alignment);

	auto &block = blocks.back();
	if (!block.blob)
		return nullptr;

	size_t offset = (block.offset + alignment - 1) & ~alignment;
	size_t required_size = offset + size;
	if (required_size <= size)
	{
		void *ret = block.blob.get() + offset;
		block.offset = required_size;
		return ret;
	}

	add_block(size + alignment);
	return allocate_raw(size, alignment);
}

void StateRecorder::set_compute_pipeline_handle(unsigned index, VkPipeline pipeline)
{
	compute_pipeline_to_index[pipeline] = index;
}

void StateRecorder::set_descriptor_set_layout_handle(unsigned index, VkDescriptorSetLayout layout)
{
	descriptor_set_layout_to_index[layout] = index;
}

void StateRecorder::set_graphics_pipeline_handle(unsigned index, VkPipeline pipeline)
{
	graphics_pipeline_to_index[pipeline] = index;
}

void StateRecorder::set_pipeline_layout_handle(unsigned index, VkPipelineLayout layout)
{
	pipeline_layout_to_index[layout] = index;
}

void StateRecorder::set_render_pass_handle(unsigned index, VkRenderPass render_pass)
{
	render_pass_to_index[render_pass] = index;
}

void StateRecorder::set_shader_module_handle(unsigned index, VkShaderModule module)
{
	shader_module_to_index[module] = index;
}

void StateRecorder::set_sampler_handle(unsigned index, VkSampler sampler)
{
	sampler_to_index[sampler] = index;
}

unsigned StateRecorder::register_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo &layout_info)
{
	auto index = unsigned(descriptor_sets.size());
	descriptor_sets.push_back({ hash, copy_descriptor_set_layout(layout_info) });
	return index;
}

unsigned StateRecorder::register_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo &layout_info)
{
	auto index = unsigned(pipeline_layouts.size());
	pipeline_layouts.push_back({ hash, copy_pipeline_layout(layout_info) });
	return index;
}

unsigned StateRecorder::register_sampler(Hash hash, const VkSamplerCreateInfo &create_info)
{
	auto index = unsigned(samplers.size());
	samplers.push_back({ hash, copy_sampler(create_info) });
	return index;
}

unsigned StateRecorder::register_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo &create_info)
{
	auto index = unsigned(graphics_pipelines.size());
	graphics_pipelines.push_back({ hash, copy_graphics_pipeline(create_info) });
	return index;
}

unsigned StateRecorder::register_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo &create_info)
{
	auto index = unsigned(compute_pipelines.size());
	compute_pipelines.push_back({ hash, copy_compute_pipeline(create_info) });
	return index;
}

unsigned StateRecorder::register_render_pass(Hash hash, const VkRenderPassCreateInfo &create_info)
{
	auto index = unsigned(render_passes.size());
	render_passes.push_back({ hash, copy_render_pass(create_info) });
	return index;
}

unsigned StateRecorder::register_shader_module(Hash hash, const VkShaderModuleCreateInfo &create_info)
{
	auto index = unsigned(shader_modules.size());
	shader_modules.push_back({ hash, copy_shader_module(create_info) });
	return index;
}

Hash StateRecorder::get_hash_for_compute_pipeline_handle(VkPipeline pipeline) const
{
	auto itr = compute_pipeline_to_index.find(pipeline);
	if (itr == end(compute_pipeline_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return compute_pipelines[itr->second].hash;
}

Hash StateRecorder::get_hash_for_graphics_pipeline_handle(VkPipeline pipeline) const
{
	auto itr = graphics_pipeline_to_index.find(pipeline);
	if (itr == end(graphics_pipeline_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return graphics_pipelines[itr->second].hash;
}

Hash StateRecorder::get_hash_for_sampler(VkSampler sampler) const
{
	auto itr = sampler_to_index.find(sampler);
	if (itr == end(sampler_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return samplers[itr->second].hash;
}

Hash StateRecorder::get_hash_for_shader_module(VkShaderModule module) const
{
	auto itr = shader_module_to_index.find(module);
	if (itr == end(shader_module_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return shader_modules[itr->second].hash;
}

Hash StateRecorder::get_hash_for_pipeline_layout(VkPipelineLayout layout) const
{
	auto itr = pipeline_layout_to_index.find(layout);
	if (itr == end(pipeline_layout_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return pipeline_layouts[itr->second].hash;
}

Hash StateRecorder::get_hash_for_descriptor_set_layout(VkDescriptorSetLayout layout) const
{
	auto itr = descriptor_set_layout_to_index.find(layout);
	if (itr == end(descriptor_set_layout_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return descriptor_sets[itr->second].hash;
}

Hash StateRecorder::get_hash_for_render_pass(VkRenderPass render_pass) const
{
	auto itr = render_pass_to_index.find(render_pass);
	if (itr == end(render_pass_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return render_passes[itr->second].hash;
}

VkShaderModuleCreateInfo StateRecorder::copy_shader_module(const VkShaderModuleCreateInfo &create_info)
{
	auto info = create_info;
	info.pCode = copy(info.pCode, info.codeSize / sizeof(uint32_t));
	return info;
}

VkSamplerCreateInfo StateRecorder::copy_sampler(const VkSamplerCreateInfo &create_info)
{
	return create_info;
}

VkDescriptorSetLayoutCreateInfo StateRecorder::copy_descriptor_set_layout(
	const VkDescriptorSetLayoutCreateInfo &create_info)
{
	auto info = create_info;
	info.pBindings = copy(info.pBindings, info.bindingCount);

	for (uint32_t i = 0; i < info.bindingCount; i++)
	{
		auto &b = info.pBindings[i];
		if (b.pImmutableSamplers &&
		    (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
		     b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
		{
			const_cast<VkSampler *>(b.pImmutableSamplers)[i] =
				reinterpret_cast<VkSampler>(uint64_t(sampler_to_index[b.pImmutableSamplers[i]] + 1));
		}
	}

	return info;
}

VkPipelineLayoutCreateInfo StateRecorder::copy_pipeline_layout(const VkPipelineLayoutCreateInfo &create_info)
{
	auto info = create_info;
	info.pPushConstantRanges = copy(info.pPushConstantRanges, info.pushConstantRangeCount);
	info.pSetLayouts = copy(info.pSetLayouts, info.setLayoutCount);
	for (uint32_t i = 0; i < info.setLayoutCount; i++)
	{
		const_cast<VkDescriptorSetLayout *>(info.pSetLayouts)[i] =
			reinterpret_cast<VkDescriptorSetLayout>(uint64_t(descriptor_set_layout_to_index[info.pSetLayouts[i]] + 1));
	}
	return info;
}

VkSpecializationInfo *StateRecorder::copy_specialization_info(const VkSpecializationInfo *info)
{
	auto *ret = copy(info, 1);
	ret->pMapEntries = copy(ret->pMapEntries, ret->mapEntryCount);
	ret->pData = copy(static_cast<const uint8_t *>(ret->pData), ret->dataSize);
	return ret;
}

VkComputePipelineCreateInfo StateRecorder::copy_compute_pipeline(const VkComputePipelineCreateInfo &create_info)
{
	auto info = create_info;
	info.stage.pSpecializationInfo = copy_specialization_info(info.stage.pSpecializationInfo);
	info.stage.module = reinterpret_cast<VkShaderModule>(uint64_t(shader_module_to_index[create_info.stage.module] + 1));
	info.stage.pName = copy(info.stage.pName, strlen(info.stage.pName) + 1);
	if (info.basePipelineHandle != VK_NULL_HANDLE)
		info.basePipelineHandle = reinterpret_cast<VkPipeline>(uint64_t(compute_pipeline_to_index[info.basePipelineHandle] + 1));
	return info;
}

VkGraphicsPipelineCreateInfo StateRecorder::copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo &create_info)
{
	auto info = create_info;

	info.pStages = copy(info.pStages, info.stageCount);
	info.pTessellationState = copy(info.pTessellationState, 1);
	info.pColorBlendState = copy(info.pColorBlendState, 1);
	info.pVertexInputState = copy(info.pVertexInputState, 1);
	info.pMultisampleState = copy(info.pMultisampleState, 1);
	info.pVertexInputState = copy(info.pVertexInputState, 1);
	info.pViewportState = copy(info.pViewportState, 1);
	info.pInputAssemblyState  = copy(info.pInputAssemblyState, 1);
	info.pDepthStencilState = copy(info.pDepthStencilState, 1);
	info.pRasterizationState = copy(info.pRasterizationState, 1);
	info.pDynamicState = copy(info.pDynamicState, 1);
	info.renderPass = reinterpret_cast<VkRenderPass>(uint64_t(render_pass_to_index[info.renderPass] + 1));
	if (info.basePipelineHandle != VK_NULL_HANDLE)
		info.basePipelineHandle = reinterpret_cast<VkPipeline>(uint64_t(graphics_pipeline_to_index[info.basePipelineHandle] + 1));

	for (uint32_t i = 0; i < info.stageCount; i++)
	{
		auto &stage = const_cast<VkPipelineShaderStageCreateInfo &>(info.pStages[i]);
		stage.pName = copy(stage.pName, strlen(stage.pName) + 1);
		stage.pSpecializationInfo = copy_specialization_info(stage.pSpecializationInfo);
		stage.module = reinterpret_cast<VkShaderModule>(uint64_t(shader_module_to_index[stage.module] + 1));
	}

	auto &blend = const_cast<VkPipelineColorBlendStateCreateInfo &>(*info.pColorBlendState);
	blend.pAttachments = copy(blend.pAttachments, blend.attachmentCount);

	auto &vs = const_cast<VkPipelineVertexInputStateCreateInfo &>(*info.pVertexInputState);
	vs.pVertexAttributeDescriptions = copy(vs.pVertexAttributeDescriptions, vs.vertexAttributeDescriptionCount);
	vs.pVertexBindingDescriptions = copy(vs.pVertexBindingDescriptions, vs.vertexBindingDescriptionCount);

	auto &ms = const_cast<VkPipelineMultisampleStateCreateInfo &>(*info.pMultisampleState);
	if (ms.pSampleMask)
		ms.pSampleMask = copy(ms.pSampleMask, (ms.rasterizationSamples + 31) / 32);

	const_cast<VkPipelineDynamicStateCreateInfo *>(info.pDynamicState)->pDynamicStates =
		copy(info.pDynamicState->pDynamicStates, info.pDynamicState->dynamicStateCount);

	return info;
}

VkRenderPassCreateInfo StateRecorder::copy_render_pass(const VkRenderPassCreateInfo &create_info)
{
	auto info = create_info;
	info.pAttachments = copy(info.pAttachments, info.attachmentCount);
	info.pSubpasses = copy(info.pSubpasses, info.subpassCount);
	info.pDependencies = copy(info.pDependencies, info.dependencyCount);

	for (uint32_t i = 0; i < info.subpassCount; i++)
	{
		auto &sub = const_cast<VkSubpassDescription &>(info.pSubpasses[i]);
		if (sub.pDepthStencilAttachment)
			sub.pDepthStencilAttachment = copy(sub.pDepthStencilAttachment, 1);
		if (sub.pColorAttachments)
			sub.pColorAttachments = copy(sub.pColorAttachments, sub.colorAttachmentCount);
		if (sub.pResolveAttachments)
			sub.pResolveAttachments = copy(sub.pResolveAttachments, sub.colorAttachmentCount);
		if (sub.pInputAttachments)
			sub.pInputAttachments = copy(sub.pInputAttachments, sub.inputAttachmentCount);
		if (sub.pPreserveAttachments)
			sub.pPreserveAttachments = copy(sub.pPreserveAttachments, sub.preserveAttachmentCount);
	}

	return info;
}

bool StateRecorder::create_device(const VkPhysicalDeviceProperties &,
                                  const VkDeviceCreateInfo &)
{
	return true;
}

static std::string encode_base64(const void *data, size_t size)
{
	char *buffer = b64_encode(static_cast<const unsigned char *>(data), size);
	std::string ret(buffer);
	free(buffer);
	return ret;
}

std::string StateRecorder::serialize() const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value samplers(kArrayType);
	for (auto &sampler : this->samplers)
	{
		Value s(kObjectType);
		s.AddMember("flags", sampler.info.flags, alloc);
		s.AddMember("minFilter", sampler.info.minFilter, alloc);
		s.AddMember("magFilter", sampler.info.magFilter, alloc);
		s.AddMember("maxAnisotropy", sampler.info.maxAnisotropy, alloc);
		s.AddMember("compareOp", sampler.info.compareOp, alloc);
		s.AddMember("anisotropyEnable", sampler.info.anisotropyEnable, alloc);
		s.AddMember("mipmapMode", sampler.info.mipmapMode, alloc);
		s.AddMember("addressModeU", sampler.info.addressModeU, alloc);
		s.AddMember("addressModeV", sampler.info.addressModeU, alloc);
		s.AddMember("addressModeW", sampler.info.addressModeU, alloc);
		s.AddMember("borderColor", sampler.info.borderColor, alloc);
		s.AddMember("unnormalizedCoordinates", sampler.info.unnormalizedCoordinates, alloc);
		s.AddMember("compareEnable", sampler.info.compareEnable, alloc);
		s.AddMember("mipLodBias", sampler.info.mipLodBias, alloc);
		s.AddMember("minLod", sampler.info.minLod, alloc);
		s.AddMember("maxLod", sampler.info.maxLod, alloc);
		samplers.PushBack(s, alloc);
	}
	doc.AddMember("samplers", samplers, alloc);

	Value set_layouts(kArrayType);
	doc.AddMember("setLayouts", set_layouts, alloc);
	for (auto &layout : descriptor_sets)
	{
		Value l(kObjectType);
		l.AddMember("hash", layout.hash, alloc);
		l.AddMember("flags", layout.info.flags, alloc);

		Value bindings(kArrayType);
		for (uint32_t i = 0; i < layout.info.bindingCount; i++)
		{
			auto &b = layout.info.pBindings[i];
			Value binding(kObjectType);
			binding.AddMember("descriptorType", b.descriptorType, alloc);
			binding.AddMember("descriptorCount", b.descriptorCount, alloc);
			binding.AddMember("stageFlags", b.stageFlags, alloc);
			binding.AddMember("binding", b.binding, alloc);
			if (b.pImmutableSamplers)
			{
				Value immutables(kArrayType);
				for (uint32_t j = 0; j < b.descriptorCount; j++)
					immutables.PushBack(reinterpret_cast<uint64_t>(b.pImmutableSamplers[j]), alloc);
				binding.AddMember("immutableSamplers", immutables, alloc);
			}
			bindings.PushBack(binding, alloc);
		}
		l.AddMember("bindings", bindings, alloc);

		set_layouts.PushBack(l, alloc);
	}

	Value pipeline_layouts(kArrayType);
	for (auto &layout : this->pipeline_layouts)
	{
		Value p(kObjectType);
		p.AddMember("hash", layout.hash, alloc);
		p.AddMember("flags", layout.info.flags, alloc);
		Value push(kArrayType);
		for (uint32_t i = 0; i < layout.info.pushConstantRangeCount; i++)
		{
			Value range(kObjectType);
			range.AddMember("stageFlags", layout.info.pPushConstantRanges[i].stageFlags, alloc);
			range.AddMember("size", layout.info.pPushConstantRanges[i].size, alloc);
			range.AddMember("offset", layout.info.pPushConstantRanges[i].offset, alloc);
			push.PushBack(range, alloc);
		}
		p.AddMember("pushConstantRanges", push, alloc);

		Value set_layouts(kArrayType);
		for (uint32_t i = 0; i < layout.info.setLayoutCount; i++)
			set_layouts.PushBack(reinterpret_cast<uint64_t>(layout.info.pSetLayouts[i]), alloc);
		p.AddMember("setLayouts", set_layouts, alloc);

		pipeline_layouts.PushBack(p, alloc);
	}
	doc.AddMember("pipelineLayouts", pipeline_layouts, alloc);

	Value shader_modules(kArrayType);
	for (auto &module : this->shader_modules)
	{
		Value m(kObjectType);
		m.AddMember("hash", module.hash, alloc);
		m.AddMember("flags", module.info.flags, alloc);
		m.AddMember("code", encode_base64(module.info.pCode, module.info.codeSize), alloc);
		shader_modules.PushBack(m, alloc);
	}
	doc.AddMember("shaderModules", shader_modules, alloc);

	Value render_passes(kArrayType);
	doc.AddMember("renderPasses", render_passes, alloc);
	for (auto &pass : this->render_passes)
	{
		Value p(kObjectType);
		p.AddMember("hash", pass.hash, alloc);
		p.AddMember("flags", pass.info.flags, alloc);

		Value deps(kArrayType);
		Value subpasses(kArrayType);
		Value attachments(kArrayType);

		for (uint32_t i = 0; i < pass.info.dependencyCount; i++)
		{
			auto &d = pass.info.pDependencies[i];
			Value dep(kObjectType);
			dep.AddMember("dependencyFlags", d.dependencyFlags, alloc);
			dep.AddMember("dstAccessMask", d.dstAccessMask, alloc);
			dep.AddMember("srcAccessMask", d.srcAccessMask, alloc);
			dep.AddMember("dstStageMask", d.dstStageMask, alloc);
			dep.AddMember("srcStageMask", d.srcStageMask, alloc);
			dep.AddMember("dstSubpass", d.dstSubpass, alloc);
			dep.AddMember("srcSubpass", d.srcSubpass, alloc);
			deps.PushBack(dep, alloc);
		}
		p.AddMember("dependencies", deps, alloc);

		for (uint32_t i = 0; i < pass.info.attachmentCount; i++)
		{
			auto &a = pass.info.pAttachments[i];
			Value att(kObjectType);

			att.AddMember("flags", a.flags, alloc);
			att.AddMember("format", a.format, alloc);
			att.AddMember("finalLayout", a.finalLayout, alloc);
			att.AddMember("initialLayout", a.initialLayout, alloc);
			att.AddMember("loadOp", a.loadOp, alloc);
			att.AddMember("storeOp", a.storeOp, alloc);
			att.AddMember("samples", a.samples, alloc);
			att.AddMember("stencilLoadOp", a.stencilLoadOp, alloc);
			att.AddMember("stencilStoreOp", a.stencilStoreOp, alloc);

			attachments.PushBack(att, alloc);
		}
		p.AddMember("attachments", attachments, alloc);

		for (uint32_t i = 0; i < pass.info.subpassCount; i++)
		{
			auto &sub = pass.info.pSubpasses[i];
			Value p(kObjectType);
			p.AddMember("flags", sub.flags, alloc);
			p.AddMember("pipelineBindPoint", sub.pipelineBindPoint, alloc);

			Value preserves(kArrayType);
			for (uint32_t i = 0; i < sub.preserveAttachmentCount; i++)
			{
				Value preserve(kObjectType);
				preserve.PushBack(sub.pPreserveAttachments[i], alloc);
			}
			p.AddMember("preserveAttachments", preserves, alloc);

			Value inputs(kArrayType);
			for (uint32_t i = 0; i < sub.inputAttachmentCount; i++)
			{
				Value input(kObjectType);
				auto &ia = sub.pInputAttachments[i];
				input.AddMember("attachment", ia.attachment, alloc);
				input.AddMember("layout", ia.layout, alloc);
				inputs.PushBack(input, alloc);
			}
			p.AddMember("inputAttachments", inputs, alloc);

			Value colors(kArrayType);
			for (uint32_t i = 0; i < sub.colorAttachmentCount; i++)
			{
				Value color(kObjectType);
				auto &c = sub.pColorAttachments[i];
				color.AddMember("attachment", c.attachment, alloc);
				color.AddMember("layout", c.layout, alloc);
				colors.PushBack(color, alloc);
			}
			p.AddMember("colorAttachments", colors, alloc);

			if (sub.pResolveAttachments)
			{
				Value resolves(kArrayType);
				for (uint32_t i = 0; i < sub.colorAttachmentCount; i++)
				{
					Value resolve(kObjectType);
					auto &r = sub.pColorAttachments[i];
					resolve.AddMember("attachment", r.attachment, alloc);
					resolve.AddMember("layout", r.layout, alloc);
					resolves.PushBack(resolve, alloc);
				}
				p.AddMember("resolveAttachments", resolves, alloc);
			}

			Value depth_stencil(kObjectType);
			if (sub.pDepthStencilAttachment)
			{
				depth_stencil.AddMember("attachment", sub.pDepthStencilAttachment->attachment, alloc);
				depth_stencil.AddMember("layout", sub.pDepthStencilAttachment->layout, alloc);
			}
			else
			{
				depth_stencil.AddMember("attachment", -1, alloc);
				depth_stencil.AddMember("layout", VK_IMAGE_LAYOUT_UNDEFINED, alloc);
			}

			subpasses.PushBack(p, alloc);
		}
		p.AddMember("subpasses", subpasses, alloc);
		render_passes.PushBack(p, alloc);
	}

	Value compute_pipelines(kArrayType);
	for (auto &pipe : this->compute_pipelines)
	{
		Value p(kObjectType);
		p.AddMember("hash", pipe.hash, alloc);
		p.AddMember("flags", pipe.info.flags, alloc);
		p.AddMember("layout", reinterpret_cast<uint64_t>(pipe.info.layout), alloc);
		p.AddMember("basePipelineHandle", reinterpret_cast<uint64_t>(pipe.info.basePipelineHandle), alloc);
		p.AddMember("basePipelineIndex", pipe.info.basePipelineIndex, alloc);
		Value stage(kObjectType);
		stage.AddMember("flags", pipe.info.stage.flags, alloc);
		stage.AddMember("stage", pipe.info.stage.stage, alloc);
		stage.AddMember("module", reinterpret_cast<uint64_t>(pipe.info.stage.module), alloc);
		stage.AddMember("name", StringRef(pipe.info.stage.pName), alloc);
		if (pipe.info.stage.pSpecializationInfo)
		{
			Value spec(kObjectType);
			spec.AddMember("dataSize", pipe.info.stage.pSpecializationInfo->dataSize, alloc);
			spec.AddMember("code",
			               encode_base64(pipe.info.stage.pSpecializationInfo->pData,
			                             pipe.info.stage.pSpecializationInfo->dataSize), alloc);
			Value map_entries(kArrayType);
			for (uint32_t i = 0; i < pipe.info.stage.pSpecializationInfo->mapEntryCount; i++)
			{
				auto &e = pipe.info.stage.pSpecializationInfo->pMapEntries[i];
				Value map_entry(kObjectType);
				map_entry.AddMember("offset", e.offset, alloc);
				map_entry.AddMember("size", e.size, alloc);
				map_entry.AddMember("constantID", e.constantID, alloc);
				map_entries.PushBack(map_entry, alloc);
			}
			spec.AddMember("mapEntries", map_entries, alloc);
			stage.AddMember("specializationInfo", spec, alloc);
		}
		p.AddMember("stage", stage, alloc);
		compute_pipelines.PushBack(p, alloc);
	}
	doc.AddMember("computePipelines", compute_pipelines, alloc);

	Value graphics_pipelines(kArrayType);
	for (auto &pipe : this->graphics_pipelines)
	{
		Value p(kObjectType);
		p.AddMember("hash", pipe.hash, alloc);
		p.AddMember("flags", pipe.info.flags, alloc);
		p.AddMember("basePipelineHandle", reinterpret_cast<uint64_t>(pipe.info.basePipelineHandle), alloc);
		p.AddMember("basePipelineIndex", pipe.info.basePipelineIndex, alloc);
		p.AddMember("layout", reinterpret_cast<uint64_t>(pipe.info.layout), alloc);
		p.AddMember("renderPass", reinterpret_cast<uint64_t>(pipe.info.renderPass), alloc);
		p.AddMember("subpass", pipe.info.subpass, alloc);

		if (pipe.info.pTessellationState)
		{
			Value tess(kObjectType);
			tess.AddMember("alloc", pipe.info.pTessellationState->flags, alloc);
			tess.AddMember("patchControlPoints", pipe.info.pTessellationState->patchControlPoints, alloc);
			p.AddMember("tessellationState", tess, alloc);
		}

		if (pipe.info.pDynamicState)
		{
			Value dyn(kObjectType);
			dyn.AddMember("flags", pipe.info.pDynamicState->flags, alloc);
			Value dynamics(kArrayType);
			for (uint32_t i = 0; i < pipe.info.pDynamicState->dynamicStateCount; i++)
				dynamics.PushBack(pipe.info.pDynamicState->pDynamicStates[i], alloc);
			dyn.AddMember("dynamicState", dynamics, alloc);
			p.AddMember("dynamicState", dyn, alloc);
		}

		if (pipe.info.pMultisampleState)
		{
			Value ms(kObjectType);
			ms.AddMember("flags", pipe.info.pMultisampleState->flags, alloc);
			ms.AddMember("rasterizationSamples", pipe.info.pMultisampleState->rasterizationSamples, alloc);
			ms.AddMember("sampleShadingEnable", pipe.info.pMultisampleState->sampleShadingEnable, alloc);
			ms.AddMember("minSampleShading", pipe.info.pMultisampleState->minSampleShading, alloc);
			ms.AddMember("alphaToOneEnable", pipe.info.pMultisampleState->alphaToOneEnable, alloc);
			ms.AddMember("alphaToCoverageEnable", pipe.info.pMultisampleState->alphaToCoverageEnable, alloc);

			Value sm(kArrayType);
			if (pipe.info.pMultisampleState->pSampleMask)
			{
				auto entries = uint32_t(pipe.info.pMultisampleState->rasterizationSamples + 31) / 32;
				for (uint32_t i = 0; i < entries; i++)
					sm.PushBack(pipe.info.pMultisampleState->pSampleMask[i], alloc);
				ms.AddMember("sampleMask", sm, alloc);
			}

			p.AddMember("multisampleState", ms, alloc);
		}

		if (pipe.info.pVertexInputState)
		{
			Value vi(kObjectType);

			Value attribs(kArrayType);
			Value bindings(kArrayType);
			vi.AddMember("flags", pipe.info.pVertexInputState->flags, alloc);

			for (uint32_t i = 0; i < pipe.info.pVertexInputState->vertexAttributeDescriptionCount; i++)
			{
				auto &a = pipe.info.pVertexInputState->pVertexAttributeDescriptions[i];
				Value attrib(kObjectType);
				attrib.AddMember("location", a.location, alloc);
				attrib.AddMember("binding", a.binding, alloc);
				attrib.AddMember("offset", a.offset, alloc);
				attrib.AddMember("format", a.format, alloc);
				attribs.PushBack(attrib, alloc);
			}

			for (uint32_t i = 0; i < pipe.info.pVertexInputState->vertexBindingDescriptionCount; i++)
			{
				auto &b = pipe.info.pVertexInputState->pVertexBindingDescriptions[i];
				Value binding(kObjectType);
				binding.AddMember("binding", b.binding, alloc);
				binding.AddMember("stride", b.stride, alloc);
				binding.AddMember("inputRate", b.inputRate, alloc);
				bindings.PushBack(binding, alloc);
			}
			vi.AddMember("attributes", attribs, alloc);
			vi.AddMember("bindings", bindings, alloc);

			p.AddMember("vertexInputState", vi, alloc);
		}

		if (pipe.info.pRasterizationState)
		{
			Value rs(kObjectType);
			rs.AddMember("flags", pipe.info.pRasterizationState->flags, alloc);
			rs.AddMember("depthBiasConstantFactor", pipe.info.pRasterizationState->depthBiasConstantFactor, alloc);
			rs.AddMember("depthBiasSlopeFactor", pipe.info.pRasterizationState->depthBiasSlopeFactor, alloc);
			rs.AddMember("depthBiasClamp", pipe.info.pRasterizationState->depthBiasClamp, alloc);
			rs.AddMember("depthBiasEnable", pipe.info.pRasterizationState->depthBiasEnable, alloc);
			rs.AddMember("depthClampEnable", pipe.info.pRasterizationState->depthClampEnable, alloc);
			rs.AddMember("polygonMode", pipe.info.pRasterizationState->polygonMode, alloc);
			rs.AddMember("rasterizerDiscardEnable", pipe.info.pRasterizationState->rasterizerDiscardEnable, alloc);
			rs.AddMember("frontFace", pipe.info.pRasterizationState->frontFace, alloc);
			rs.AddMember("lineWidth", pipe.info.pRasterizationState->lineWidth, alloc);
			rs.AddMember("cullMode", pipe.info.pRasterizationState->cullMode, alloc);
			p.AddMember("rasterizationState", rs, alloc);
		}

		if (pipe.info.pInputAssemblyState)
		{
			Value ia(kObjectType);
			ia.AddMember("flags", pipe.info.pInputAssemblyState->flags, alloc);
			ia.AddMember("topology", pipe.info.pInputAssemblyState->topology, alloc);
			ia.AddMember("primitiveRestartEnable", pipe.info.pInputAssemblyState->primitiveRestartEnable, alloc);
			p.AddMember("inputAssemblyState", ia, alloc);
		}

		if (pipe.info.pColorBlendState)
		{
			Value cb(kObjectType);
			cb.AddMember("flags", pipe.info.pColorBlendState->flags, alloc);
			cb.AddMember("logicOp", pipe.info.pColorBlendState->logicOp, alloc);
			cb.AddMember("logicOpEnable", pipe.info.pColorBlendState->logicOpEnable, alloc);
			Value blend_constants(kArrayType);
			for (auto &c : pipe.info.pColorBlendState->blendConstants)
				blend_constants.PushBack(c, alloc);
			cb.AddMember("blendConstants", blend_constants, alloc);
			Value attachments(kArrayType);
			for (uint32_t i = 0; i < pipe.info.pColorBlendState->attachmentCount; i++)
			{
				auto &a = pipe.info.pColorBlendState->pAttachments[i];
				Value att(kObjectType);
				att.AddMember("dstAlphaBlendFactor", a.dstAlphaBlendFactor, alloc);
				att.AddMember("srcAlphaBlendFactor", a.srcAlphaBlendFactor, alloc);
				att.AddMember("dstColorBlendFactor", a.dstColorBlendFactor, alloc);
				att.AddMember("srcColorBlendFactor", a.srcColorBlendFactor, alloc);
				att.AddMember("colorWriteMask", a.colorWriteMask, alloc);
				att.AddMember("alphaBlendOp", a.alphaBlendOp, alloc);
				att.AddMember("colorBlendOp", a.colorBlendOp, alloc);
				att.AddMember("blendEnable", a.blendEnable, alloc);
			}
			cb.AddMember("attachments", attachments, alloc);
			p.AddMember("colorBlendState", cb, alloc);
		}

		if (pipe.info.pViewportState)
		{
			Value vp(kObjectType);
			vp.AddMember("flags", pipe.info.pViewportState->flags, alloc);
			if (pipe.info.pViewportState->pViewports)
			{
				Value viewports(kArrayType);
				for (uint32_t i = 0; i < pipe.info.pViewportState->viewportCount; i++)
				{
					Value viewport(kObjectType);
					viewport.AddMember("x", pipe.info.pViewportState->pViewports[i].x, alloc);
					viewport.AddMember("y", pipe.info.pViewportState->pViewports[i].y, alloc);
					viewport.AddMember("width", pipe.info.pViewportState->pViewports[i].width, alloc);
					viewport.AddMember("height", pipe.info.pViewportState->pViewports[i].height, alloc);
					viewport.AddMember("minDepth", pipe.info.pViewportState->pViewports[i].minDepth, alloc);
					viewport.AddMember("maxDepth", pipe.info.pViewportState->pViewports[i].maxDepth, alloc);
					viewports.PushBack(viewport, alloc);
				}
				vp.AddMember("viewports", viewports, alloc);
			}

			if (pipe.info.pViewportState->pScissors)
			{
				Value scissors(kArrayType);
				for (uint32_t i = 0; i < pipe.info.pViewportState->scissorCount; i++)
				{
					Value scissor(kObjectType);
					scissor.AddMember("x", pipe.info.pViewportState->pScissors[i].offset.x, alloc);
					scissor.AddMember("y", pipe.info.pViewportState->pScissors[i].offset.y, alloc);
					scissor.AddMember("width", pipe.info.pViewportState->pScissors[i].extent.width, alloc);
					scissor.AddMember("height", pipe.info.pViewportState->pScissors[i].extent.height, alloc);
					scissors.PushBack(scissor, alloc);
				}
				vp.AddMember("scissors", scissors, alloc);
			}
			p.AddMember("viewportState", vp, alloc);
		}

		if (pipe.info.pDepthStencilState)
		{
			Value ds(kObjectType);
			ds.AddMember("flags", pipe.info.pDepthStencilState->flags, alloc);
			ds.AddMember("stencilTestEnable", pipe.info.pDepthStencilState->stencilTestEnable, alloc);
			ds.AddMember("maxDepthBounds", pipe.info.pDepthStencilState->maxDepthBounds, alloc);
			ds.AddMember("minDepthBounds", pipe.info.pDepthStencilState->minDepthBounds, alloc);
			ds.AddMember("depthBoundsTestEnable", pipe.info.pDepthStencilState->depthBoundsTestEnable, alloc);
			ds.AddMember("depthWriteEnable", pipe.info.pDepthStencilState->depthWriteEnable, alloc);
			ds.AddMember("depthTestEnable", pipe.info.pDepthStencilState->depthTestEnable, alloc);
			ds.AddMember("depthCompareOp", pipe.info.pDepthStencilState->depthCompareOp, alloc);

			const auto serialize_stencil = [&](Value &v, const VkStencilOpState &state) {
				v.AddMember("compareOp", state.compareOp, alloc);
				v.AddMember("writeMask", state.writeMask, alloc);
				v.AddMember("reference", state.reference, alloc);
				v.AddMember("compareMask", state.compareMask, alloc);
				v.AddMember("passOp", state.passOp, alloc);
				v.AddMember("failOp", state.failOp, alloc);
				v.AddMember("depthFailOp", state.depthFailOp, alloc);
			};
			Value front(kObjectType);
			Value back(kObjectType);
			serialize_stencil(front, pipe.info.pDepthStencilState->front);
			serialize_stencil(back, pipe.info.pDepthStencilState->back);
			ds.AddMember("front", front, alloc);
			ds.AddMember("back", back, alloc);
			p.AddMember("depthStencilState", ds, alloc);
		}

		Value stages(kArrayType);
		for (uint32_t i = 0; i < pipe.info.stageCount; i++)
		{
			auto &s = pipe.info.pStages[i];
			Value stage(kObjectType);
			stage.AddMember("flags", s.flags, alloc);
			stage.AddMember("name", StringRef(s.pName), alloc);
			stage.AddMember("module", reinterpret_cast<uint64_t>(s.module), alloc);
			stage.AddMember("stage", s.stage, alloc);
			if (s.pSpecializationInfo)
			{
				Value spec(kObjectType);
				spec.AddMember("dataSize", s.pSpecializationInfo->dataSize, alloc);
				spec.AddMember("code",
				               encode_base64(s.pSpecializationInfo->pData,
				                             s.pSpecializationInfo->dataSize), alloc);
				Value map_entries(kArrayType);
				for (uint32_t i = 0; i < s.pSpecializationInfo->mapEntryCount; i++)
				{
					auto &e = s.pSpecializationInfo->pMapEntries[i];
					Value map_entry(kObjectType);
					map_entry.AddMember("offset", e.offset, alloc);
					map_entry.AddMember("size", e.size, alloc);
					map_entry.AddMember("constantID", e.constantID, alloc);
					map_entries.PushBack(map_entry, alloc);
				}
				spec.AddMember("mapEntries", map_entries, alloc);
				stage.AddMember("specializationInfo", spec, alloc);
			}
			stages.PushBack(stage, alloc);
		}
		p.AddMember("stages", stages, alloc);

		graphics_pipelines.PushBack(p, alloc);
	}
	doc.AddMember("graphicsPipelines", graphics_pipelines, alloc);

	StringBuffer buffer;
	PrettyWriter<StringBuffer> writer(buffer);
	doc.Accept(writer);
	return buffer.GetString();
}

}