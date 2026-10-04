#include "application.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

namespace application {

namespace {

struct Vertex {
	glm::vec3 position;
	glm::vec3 color;
};

std::array<Vertex, 6> octahedron_vertices = {{
	{{0.0f, 1.0f, 0.0f}, {}},
	{{0.0f, -1.0f, 0.0f}, {}},
	{{1.0f, 0.0f, 0.0f}, {}},
	{{0.0f, 0.0f, 1.0f}, {}},
	{{-1.0f, 0.0f, 0.0f}, {}},
	{{0.0f, 0.0f, -1.0f}, {}},
}};

// CCW
const std::array<uint16_t, 24> octahedron_indices = {
	// Top
	0, 3, 2, // +Z, +X
	0, 4, 3, // -X, +Z
	0, 5, 4, // -Z, -X
	0, 2, 5, // +X, -Z

	// Bottom
	1, 2, 3, // +X, +Z
	1, 3, 4, // +Z, -X
	1, 4, 5, // -X, -Z
	1, 5, 2, // -Z, +X
};

struct UniformBufferObject {
	alignas(16) glm::mat4 mvp;
	alignas(16) glm::vec4 tintColor;
};

VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;

VkBuffer vertex_buffer = VK_NULL_HANDLE;
VmaAllocation vertex_buffer_alloc = VK_NULL_HANDLE;

VkBuffer index_buffer = VK_NULL_HANDLE;
VmaAllocation index_buffer_alloc = VK_NULL_HANDLE;

VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkBuffer ubo_buffer = VK_NULL_HANDLE;
VmaAllocation ubo_alloc = VK_NULL_HANDLE;
void *ubo_mapped_data = nullptr;
VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

// Scene parameters
int current_proj_item = 0; // 0 - Perspective, 1 - Orthographic
float fov = 45.0f;
float ortho_size = 3.0f;

glm::vec3 obj_pos = {0.0f, 0.0f, 0.0f};
glm::vec3 obj_rot = {0.0f, 0.0f, 0.0f};
glm::vec3 obj_scale = {1.0f, 1.0f, 1.0f};

glm::vec3 tint_color = {1.0f, 1.0f, 1.0f};

bool enable_trajectory = true;
bool anim_playing = true;
float anim_speed = 1.0f;
float trajectory_radius = 2.5f;
float anim_time = 0.0f;
double last_time = 0.0;

std::vector<char> readShaderFile(const std::string &filename) {
	std::ifstream file(filename, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		file.open("../" + filename, std::ios::ate | std::ios::binary);
	}
	if (!file.is_open()) {
		std::cerr << "Failed to open shader file: " << filename << std::endl;
		return {};
	}
	const size_t file_size = static_cast<size_t>(file.tellg());
	std::vector<char> buffer(file_size);
	file.seekg(0);
	file.read(buffer.data(), file_size);
	return buffer;
}

VkShaderModule createShaderModule(VkDevice device,
								  const std::vector<char> &code) {
	const VkShaderModuleCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code.size(),
		.pCode = reinterpret_cast<const uint32_t *>(code.data()),
	};

	VkShaderModule shader_module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &create_info, nullptr, &shader_module) !=
		VK_SUCCESS) {
		std::cerr << "Failed to create VkShaderModule\n";
	}
	return shader_module;
}

bool createBuffer(VmaAllocator allocator, VkDeviceSize size,
				  VkBufferUsageFlags usage, VkBuffer &buffer,
				  VmaAllocation &allocation) {
	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	const VmaAllocationCreateInfo alloc_info = {
		.usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
	};

	return vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &buffer,
						   &allocation, nullptr) == VK_SUCCESS;
}

} // namespace

bool initialize() {
	auto &context = graphics::internal::context;

	for (auto &vertex : octahedron_vertices) {
		vertex.color = 0.5f * vertex.position + glm::vec3(0.5f);
	}

	// Buffers
	const VkDeviceSize vertex_buffer_size =
		sizeof(octahedron_vertices[0]) * octahedron_vertices.size();
	if (!createBuffer(context.allocator, vertex_buffer_size,
					  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer,
					  vertex_buffer_alloc)) {
		std::cerr << "Failed to create vertex buffer\n";
		return false;
	}

	void *mapped_data = nullptr;
	vmaMapMemory(context.allocator, vertex_buffer_alloc, &mapped_data);
	std::memcpy(mapped_data, octahedron_vertices.data(), vertex_buffer_size);
	vmaFlushAllocation(context.allocator, vertex_buffer_alloc, 0,
					   vertex_buffer_size);
	vmaUnmapMemory(context.allocator, vertex_buffer_alloc);

	const VkDeviceSize index_buffer_size =
		sizeof(octahedron_indices[0]) * octahedron_indices.size();

	if (!createBuffer(context.allocator, index_buffer_size,
					  VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer,
					  index_buffer_alloc)) {
		std::cerr << "Failed to create index buffer\n";
		return false;
	}

	vmaMapMemory(context.allocator, index_buffer_alloc, &mapped_data);
	std::memcpy(mapped_data, octahedron_indices.data(), index_buffer_size);
	vmaFlushAllocation(context.allocator, index_buffer_alloc, 0,
					   index_buffer_size);
	vmaUnmapMemory(context.allocator, index_buffer_alloc);

	// UBO
	if (!createBuffer(context.allocator, sizeof(UniformBufferObject),
					  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ubo_buffer,
					  ubo_alloc)) {
		std::cerr << "Failed to create UBO buffer\n";
		return false;
	}
	vmaMapMemory(context.allocator, ubo_alloc, &ubo_mapped_data);

	// Descriptor Set
	const VkDescriptorSetLayoutBinding ubo_layout_binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.pImmutableSamplers = nullptr,
	};

	const VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &ubo_layout_binding,
	};

	if (vkCreateDescriptorSetLayout(context.device, &descriptor_layout_info,
									nullptr,
									&descriptor_set_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create Descriptor Set Layout\n";
		return false;
	}

	// UBO Descriptor Pool
	const VkDescriptorPoolSize pool_size = {
		.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
	};

	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};

	if (vkCreateDescriptorPool(context.device, &pool_info, nullptr,
							   &descriptor_pool) != VK_SUCCESS) {
		std::cerr << "Failed to create Descriptor Pool\n";
		return false;
	}

	const VkDescriptorSetAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &descriptor_set_layout,
	};

	if (vkAllocateDescriptorSets(context.device, &alloc_info,
								 &descriptor_set) != VK_SUCCESS) {
		std::cerr << "Failed to allocate Descriptor Sets\n";
		return false;
	}

	const VkDescriptorBufferInfo buffer_info = {
		.buffer = ubo_buffer,
		.offset = 0,
		.range = sizeof(UniformBufferObject),
	};

	const VkWriteDescriptorSet descriptor_write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptor_set,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &buffer_info,
	};

	vkUpdateDescriptorSets(context.device, 1, &descriptor_write, 0, nullptr);

	// Shaders reading
	const auto vert_code = readShaderFile("shaders/octahedron.vert.spv");
	const auto frag_code = readShaderFile("shaders/octahedron.frag.spv");

	if (vert_code.empty() || frag_code.empty()) {
		std::cerr
			<< "Shader bytecode is empty or shader files were not found\n";
		return false;
	}

	const VkShaderModule vert_module =
		createShaderModule(context.device, vert_code);
	const VkShaderModule frag_module =
		createShaderModule(context.device, frag_code);

	const VkPipelineShaderStageCreateInfo shader_stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vert_module,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = frag_module,
			.pName = "main",
		},
	};

	// Pipeline
	const VkPipelineLayoutCreateInfo pipeline_layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &descriptor_set_layout,
	};

	if (vkCreatePipelineLayout(context.device, &pipeline_layout_info, nullptr,
							   &pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create Pipeline Layout\n";
		return false;
	}

	const VkVertexInputBindingDescription binding_desc = {
		.binding = 0,
		.stride = sizeof(Vertex),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};

	const VkVertexInputAttributeDescription attrib_descs[] = {
		{
			.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, position),
		},
		{
			.location = 1,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, color),
		},
	};

	const VkPipelineVertexInputStateCreateInfo vertex_input_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &binding_desc,
		.vertexAttributeDescriptionCount = 2,
		.pVertexAttributeDescriptions = attrib_descs,
	};

	const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE,
	};

	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	const VkPipelineRasterizationStateCreateInfo rasterizer = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.depthClampEnable = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthBiasEnable = VK_FALSE,
		.lineWidth = 1.0f,
	};

	const VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = VK_FALSE,
	};

	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable = VK_FALSE,
	};

	const VkPipelineColorBlendAttachmentState color_blend_attachment = {
		.blendEnable = VK_FALSE,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
						  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};

	const VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.logicOpEnable = VK_FALSE,
		.attachmentCount = 1,
		.pAttachments = &color_blend_attachment,
	};

	const VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};

	const VkPipelineDynamicStateCreateInfo dynamic_state_ci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic_states,
	};

	const VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = shader_stages,
		.pVertexInputState = &vertex_input_info,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &color_blending,
		.pDynamicState = &dynamic_state_ci,
		.layout = pipeline_layout,
		.renderPass = context.render_pass,
		.subpass = 0,
	};

	if (vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1,
								  &pipeline_info, nullptr,
								  &pipeline) != VK_SUCCESS) {
		std::cerr << "Failed to create graphics pipeline (VkPipeline)\n";
		vkDestroyShaderModule(context.device, frag_module, nullptr);
		vkDestroyShaderModule(context.device, vert_module, nullptr);
		return false;
	}

	vkDestroyShaderModule(context.device, frag_module, nullptr);
	vkDestroyShaderModule(context.device, vert_module, nullptr);

	return true;
}

void shutdown() {
	auto &context = graphics::internal::context;
	vkQueueWaitIdle(context.graphics_queue);

	if (ubo_mapped_data != nullptr) {
		vmaUnmapMemory(context.allocator, ubo_alloc);
		ubo_mapped_data = nullptr;
	}
	if (ubo_buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(context.allocator, ubo_buffer, ubo_alloc);
		ubo_buffer = VK_NULL_HANDLE;
	}

	if (descriptor_pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
		descriptor_pool = VK_NULL_HANDLE;
	}

	if (descriptor_set_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(context.device, descriptor_set_layout,
									 nullptr);
		descriptor_set_layout = VK_NULL_HANDLE;
	}

	if (pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(context.device, pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
	}

	if (pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
		pipeline_layout = VK_NULL_HANDLE;
	}

	if (index_buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(context.allocator, index_buffer, index_buffer_alloc);
		index_buffer = VK_NULL_HANDLE;
	}

	if (vertex_buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(context.allocator, vertex_buffer, vertex_buffer_alloc);
		vertex_buffer = VK_NULL_HANDLE;
	}
}

void update(double time) {
	// delta time
	double dt = 0.0;
	if (last_time > 0.0) {
		dt = time - last_time;
	}
	last_time = time;

	if (anim_playing) {
		anim_time += static_cast<float>(dt) * anim_speed;
	}

	// ImGUI
	ImGui::Begin("Regular Octahedron");

	if (ImGui::CollapsingHeader("Projection matrix",
								ImGuiTreeNodeFlags_DefaultOpen)) {
		const char *proj_items[] = {"Perspective", "Orthographic"};
		ImGui::Combo("Projection mode", &current_proj_item, proj_items, 2);
		if (current_proj_item == 0) {
			ImGui::SliderFloat("Field of view (FOV)", &fov, 20.0f, 110.0f);
		} else {
			ImGui::SliderFloat("Orthographic size", &ortho_size, 0.5f, 10.0f);
		}
	}

	if (ImGui::CollapsingHeader("Object transforms",
								ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::DragFloat3("Position (X, Y, Z)", glm::value_ptr(obj_pos), 0.05f);
		ImGui::SliderFloat3("Rotation (Pitch, Yaw, Roll)",
							glm::value_ptr(obj_rot), -180.0f, 180.0f);
		ImGui::DragFloat3("Scale", glm::value_ptr(obj_scale), 0.05f, 0.1f,
						  10.0f);
		if (ImGui::Button("Reset transforms")) {
			obj_pos = {0.0f, 0.0f, 0.0f};
			obj_rot = {0.0f, 0.0f, 0.0f};
			obj_scale = {1.0f, 1.0f, 1.0f};
		}
	}

	if (ImGui::CollapsingHeader("Path animation",
								ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable path", &enable_trajectory);
		ImGui::SameLine();
		if (ImGui::Button(anim_playing ? " Pause " : "Start")) {
			anim_playing = !anim_playing;
		}
		ImGui::SliderFloat("Animation speed", &anim_speed, 0.0f, 5.0f);
		ImGui::SliderFloat("Path radius", &trajectory_radius, 0.5f, 8.0f);
	}

	if (ImGui::CollapsingHeader("Object colors",
								ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::ColorEdit3("Octahedron color", glm::value_ptr(tint_color));
		ImGui::TextDisabled(
			"Tint multiplies the procedural vertex color gradient.");
	}

	ImGui::End();

	// MVP matrix calculation
	const auto extent = graphics::internal::context.swapchain_extent;
	float aspect =
		static_cast<float>(extent.width) / static_cast<float>(extent.height);
	if (aspect <= 0.0f) {
		aspect = 1.0f;
	}

	const glm::vec3 camera_pos = {0.0f, 2.5f, 6.0f};
	const glm::vec3 camera_target = {0.0f, 0.0f, 0.0f};
	const glm::vec3 camera_up = {0.0f, 1.0f, 0.0f};
	const glm::mat4 view = glm::lookAt(camera_pos, camera_target, camera_up);

	glm::mat4 proj;
	if (current_proj_item == 0) {
		proj = glm::perspective(glm::radians(fov), aspect, 0.1f, 100.0f);
	} else {
		proj = glm::ortho(-ortho_size * aspect, ortho_size * aspect,
						  -ortho_size, ortho_size, 0.1f, 100.0f);
	}
	proj[1][1] *= -1.0f; // Invert Y

	glm::vec3 cur_pos = obj_pos;
	glm::vec3 cur_rot = obj_rot;

	if (enable_trajectory) {
		// Update position and rotation based on animation time
		cur_pos.x += trajectory_radius * std::cos(anim_time);
		cur_pos.z += trajectory_radius * std::sin(anim_time);
		cur_pos.y += 0.5f * trajectory_radius * std::sin(2.0f * anim_time);
		cur_rot.y += glm::degrees(anim_time * 1.5f);
		cur_rot.x += glm::degrees(anim_time * 0.8f);
	}

	glm::mat4 model = glm::mat4(1.0f);
	model = glm::translate(model, cur_pos);
	model = glm::rotate(model, glm::radians(cur_rot.x),
						glm::vec3(1.0f, 0.0f, 0.0f));
	model = glm::rotate(model, glm::radians(cur_rot.y),
						glm::vec3(0.0f, 1.0f, 0.0f));
	model = glm::rotate(model, glm::radians(cur_rot.z),
						glm::vec3(0.0f, 0.0f, 1.0f));
	model = glm::scale(model, obj_scale);

	UniformBufferObject ubo{};
	ubo.mvp = proj * view * model;
	ubo.tintColor = glm::vec4(tint_color, 1.0f);
	std::memcpy(ubo_mapped_data, &ubo, sizeof(ubo));
	vmaFlushAllocation(graphics::internal::context.allocator, ubo_alloc, 0,
					   sizeof(ubo));
}

void render(const graphics::internal::FrameData &fd) {
	vkResetCommandBuffer(fd.command_buffer, 0);

	const VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vkBeginCommandBuffer(fd.command_buffer, &begin_info);

	const VkClearValue clear_values[] = {
		{.color = {{0.08f, 0.08f, 0.11f, 1.0f}}},
		{.depthStencil = {1.0f, 0}},
	};

	const VkRenderPassBeginInfo render_pass_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = graphics::internal::context.render_pass,
		.framebuffer = fd.framebuffer,
		.renderArea = {.extent = graphics::internal::context.swapchain_extent},
		.clearValueCount = sizeof(clear_values) / sizeof(clear_values[0]),
		.pClearValues = clear_values,
	};

	vkCmdBeginRenderPass(fd.command_buffer, &render_pass_begin,
						 VK_SUBPASS_CONTENTS_INLINE);

	const auto extent = graphics::internal::context.swapchain_extent;
	const VkViewport viewport = {
		.x = 0.0f,
		.y = 0.0f,
		.width = static_cast<float>(extent.width),
		.height = static_cast<float>(extent.height),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);

	const VkRect2D scissor = {
		.offset = {0, 0},
		.extent = extent,
	};
	vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);

	vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
					  pipeline);

	const VkDeviceSize offsets[] = {0};
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer, offsets);
	vkCmdBindIndexBuffer(fd.command_buffer, index_buffer, 0,
						 VK_INDEX_TYPE_UINT16);

	// UBO binding and draw
	vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
							pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
	vkCmdDrawIndexed(fd.command_buffer,
					 static_cast<uint32_t>(octahedron_indices.size()), 1, 0, 0,
					 0);

	vkCmdEndRenderPass(fd.command_buffer);

	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application