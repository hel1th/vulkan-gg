#include "application.hpp"
#include "upload.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
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
	glm::vec3 normal;
	glm::vec2 uv;
};

const std::array<glm::vec3, 6> octahedron_positions = {{
	{0.0f, 1.0f, 0.0f},
	{0.0f, -1.0f, 0.0f},
	{1.0f, 0.0f, 0.0f},
	{0.0f, 0.0f, 1.0f},
	{-1.0f, 0.0f, 0.0f},
	{0.0f, 0.0f, -1.0f},
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

struct SceneData {
	glm::mat4 view_projection;
	glm::vec4 camera;
	glm::vec4 ambient;
	glm::vec4 light_direction;
	glm::vec4 light_color;
	glm::ivec4 light_counts;
};

struct ModelData {
	glm::mat4 model;
	glm::mat4 normal_matrix;
	glm::vec4 specular;
};

struct PointLight {
	glm::vec4 position = {2.0f, 1.5f, 2.0f, 0.0f};
	glm::vec4 color = {1.0f, 0.65f, 0.3f, 3.0f};
};

struct SpotLight {
	PointLight light = {{-2.0f, 2.0f, 2.0f, 0.0f}, {0.4f, 0.65f, 1.0f, 6.0f}};
	glm::vec4 direction = {1.0f, -1.0f, -1.0f, 0.0f};
	glm::vec4 angles = {18.0f, 30.0f, 0.0f, 0.0f};
};

struct TextureEffect {
	float time;
	uint32_t enabled;
};

constexpr uint32_t texture_count = 2;
constexpr uint32_t object_count = 2;
const char *texture_names[texture_count] = {"Texture 1", "Texture 2"};
const char *texture_paths[texture_count] = {
	TEXTURE_DIRECTORY "/texture1",
	TEXTURE_DIRECTORY "/texture2",
};
Texture textures[texture_count];
VkSampler texture_sampler = VK_NULL_HANDLE;
int selected_textures[object_count] = {0, 1};
bool animated_texture = false;
float texture_time = 0.0f;

VkDescriptorSetLayout descriptor_layouts[2]{};
VkDescriptorSet descriptor_sets[1 + texture_count]{};
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;

DataBuffer vertex_buffer, index_buffer, scene_buffer, model_buffer, point_buffer, spot_buffer;
uint32_t model_stride = 0;
constexpr uint32_t octahedron_count = octahedron_indices.size();

// Camera
glm::vec3 camera_pos = {0.0f, 2.5f, 6.0f};
float camera_yaw = 0.0f;
float camera_pitch = -22.62f;
int current_proj_item = 0; // 0 - Perspective, 1 - Orthographic
float fov = 45.0f;
float ortho_size = 3.0f;

// Object
glm::vec3 obj_pos = {0.0f, 0.0f, 0.0f};
glm::vec3 obj_rot = {0.0f, 0.0f, 0.0f};
glm::vec3 obj_scale = {1.0f, 1.0f, 1.0f};

glm::vec3 specular_color = {1.0f, 1.0f, 1.0f};
float shininess = 32.0f;

bool enable_trajectory = false;
bool anim_playing = true;
float anim_speed = 1.0f;
float trajectory_radius = 2.5f;
float anim_time = 0.0f;
double last_time = 0.0;

// Lights
constexpr size_t max_lights = 32;
glm::vec4 ambient = {1.0f, 1.0f, 1.0f, 0.15f};
glm::vec4 light_direction = {-1.0f, -1.0f, -1.0f, 0.0f};
glm::vec4 light_color = {1.0f, 1.0f, 1.0f, 0.65f};
std::vector<PointLight> point_lights = {PointLight{}};
std::vector<SpotLight> spot_lights = {SpotLight{}};

std::vector<char> readShaderFile(const char *filename) {
	const std::string path = std::string(SHADER_DIRECTORY) + '/' + filename;
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		std::cerr << "Failed to open shader file: " << path << std::endl;
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

std::array<Vertex, octahedron_count> makeVertices() {
	std::array<Vertex, octahedron_count> vertices{};
	for (size_t i = 0; i < octahedron_count; i += 3) {
		const auto a = octahedron_positions[octahedron_indices[i]];
		const auto b = octahedron_positions[octahedron_indices[i + 1]];
		const auto c = octahedron_positions[octahedron_indices[i + 2]];
		const auto normal = glm::normalize(glm::cross(b - a, c - a));
		const bool top = i < octahedron_count / 2;
		vertices[i] = {a, normal, {0.5f, top ? 0.0f : 1.0f}};
		vertices[i + 1] = {b, normal, {top ? 0.0f : 1.0f, 0.5f}};
		vertices[i + 2] = {c, normal, {top ? 1.0f : 0.0f, 0.5f}};
	}
	return vertices;
}

void editLightColor(glm::vec4 &color) {
	ImGui::ColorEdit3("Color", glm::value_ptr(color));
	ImGui::SliderFloat("Intensity", &color.w, 0.0f, 20.0f);
}

void editPointLight(PointLight &light) {
	ImGui::DragFloat3("Position", glm::value_ptr(light.position), 0.05f);
	editLightColor(light.color);
}

} // namespace

bool initialize() {
	auto &context = graphics::internal::context;

	// Buffers
	const auto vertices = makeVertices();
	std::array<uint16_t, octahedron_count> indices{};
	std::iota(indices.begin(), indices.end(), uint16_t{0});
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(context.physical_device, &properties);
	const auto alignment = properties.limits.minUniformBufferOffsetAlignment;
	model_stride = static_cast<uint32_t>(
		(sizeof(ModelData) + alignment - 1) / alignment * alignment);
	const VkDeviceSize point_size = sizeof(PointLight) * max_lights;
	const VkDeviceSize spot_size = sizeof(SpotLight) * max_lights;
	if (!uploadBuffer(vertex_buffer, vertices.data(), sizeof(vertices),
					  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
		!uploadBuffer(index_buffer, indices.data(), sizeof(indices),
					  VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
		!createBuffer(scene_buffer, sizeof(SceneData),
					  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
		!createBuffer(model_buffer, model_stride * object_count,
					  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
		!createBuffer(point_buffer, point_size,
					  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
		!createBuffer(spot_buffer, spot_size,
					  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
		std::cerr << "Failed to create buffers\n";
		return false;
	}

	// Textures
	for (uint32_t i = 0; i < texture_count; ++i) {
		if (!loadTexture(textures[i], texture_paths[i])) {
			return false;
		}
	}
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
	};
	if (vkCreateSampler(context.device, &sampler_info, nullptr, &texture_sampler) != VK_SUCCESS) {
		return false;
	}

	// Descriptor sets
	const VkShaderStageFlags both_stages =
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	const VkDescriptorSetLayoutBinding scene_bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both_stages, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
		 nullptr},
		{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
		 nullptr},
	};
	const VkDescriptorSetLayoutBinding material_bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both_stages, nullptr},
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
	};

	VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 3,
		.pBindings = scene_bindings,
	};
	if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr,
									&descriptor_layouts[0]) != VK_SUCCESS) {
		return false;
	}
	layout_info.bindingCount = 2;
	layout_info.pBindings = material_bindings;
	if (vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr,
									&descriptor_layouts[1]) != VK_SUCCESS) {
		return false;
	}

	const VkDescriptorPoolSize pool_sizes[] = {
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, texture_count},
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture_count},
	};
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1 + texture_count,
		.poolSizeCount = 4,
		.pPoolSizes = pool_sizes,
	};
	if (vkCreateDescriptorPool(context.device, &pool_info, nullptr,
							   &descriptor_pool) != VK_SUCCESS) {
		return false;
	}
	std::array<VkDescriptorSetLayout, 1 + texture_count> set_layouts;
	set_layouts.fill(descriptor_layouts[1]);
	set_layouts[0] = descriptor_layouts[0];
	const VkDescriptorSetAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptor_pool,
		.descriptorSetCount = 1 + texture_count,
		.pSetLayouts = set_layouts.data(),
	};
	if (vkAllocateDescriptorSets(context.device, &alloc_info,
								 descriptor_sets) != VK_SUCCESS) {
		return false;
	}

	const VkDescriptorBufferInfo buffer_infos[] = {
		{scene_buffer.buffer, 0, sizeof(SceneData)},
		{point_buffer.buffer, 0, point_size},
		{spot_buffer.buffer, 0, spot_size},
	};
	VkWriteDescriptorSet writes[3]{};
	for (uint32_t i = 0; i < 3; ++i) {
		writes[i] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = descriptor_sets[0],
			.dstBinding = i,
			.descriptorCount = 1,
			.descriptorType = scene_bindings[i].descriptorType,
			.pBufferInfo = &buffer_infos[i],
		};
	}
	vkUpdateDescriptorSets(context.device, 3, writes, 0, nullptr);

	const VkDescriptorBufferInfo model_info{model_buffer.buffer, 0, sizeof(ModelData)};
	for (uint32_t i = 0; i < texture_count; ++i) {
		const VkDescriptorImageInfo image_info = {
			texture_sampler, textures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		const VkWriteDescriptorSet material_writes[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_sets[i + 1],
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
				.pBufferInfo = &model_info,
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_sets[i + 1],
				.dstBinding = 1,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_info,
			},
		};
		vkUpdateDescriptorSets(context.device, 2, material_writes, 0, nullptr);
	}

	// Shaders reading
	const auto vert_code = readShaderFile("octahedron.vert.spv");
	const auto frag_code = readShaderFile("octahedron.frag.spv");

	if (vert_code.empty() || frag_code.empty()) {
		std::cerr
			<< "Shader bytecode is empty or shader files were not found\n";
		return false;
	}

	const VkShaderModule vert_module =
		createShaderModule(context.device, vert_code);
	const VkShaderModule frag_module =
		createShaderModule(context.device, frag_code);

	if (vert_module == VK_NULL_HANDLE || frag_module == VK_NULL_HANDLE) {
		vkDestroyShaderModule(context.device, frag_module, nullptr);
		vkDestroyShaderModule(context.device, vert_module, nullptr);
		return false;
	}

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
	const VkPushConstantRange effect_range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.size = sizeof(TextureEffect),
	};
	const VkPipelineLayoutCreateInfo pipeline_layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2,
		.pSetLayouts = descriptor_layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &effect_range,
	};

	if (vkCreatePipelineLayout(context.device, &pipeline_layout_info, nullptr,
							   &pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create Pipeline Layout\n";
		vkDestroyShaderModule(context.device, frag_module, nullptr);
		vkDestroyShaderModule(context.device, vert_module, nullptr);
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
			.offset = offsetof(Vertex, normal),
		},
		{
			.location = 2,
			.binding = 0,
			.format = VK_FORMAT_R32G32_SFLOAT,
			.offset = offsetof(Vertex, uv),
		},
	};

	const VkPipelineVertexInputStateCreateInfo vertex_input_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &binding_desc,
		.vertexAttributeDescriptionCount = 3,
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

	const VkResult result = vkCreateGraphicsPipelines(
		context.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
	vkDestroyShaderModule(context.device, frag_module, nullptr);
	vkDestroyShaderModule(context.device, vert_module, nullptr);
	return result == VK_SUCCESS;
}

void shutdown() {
	auto &context = graphics::internal::context;
	vkQueueWaitIdle(context.graphics_queue);

	vkDestroyPipeline(context.device, pipeline, nullptr);
	vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
	vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
	vkDestroySampler(context.device, texture_sampler, nullptr);
	for (const auto &texture : textures) {
		vkDestroyImageView(context.device, texture.view, nullptr);
		if (texture.image != VK_NULL_HANDLE) {
			vmaDestroyImage(context.allocator, texture.image, texture.allocation);
		}
	}
	for (auto layout : descriptor_layouts) {
		vkDestroyDescriptorSetLayout(context.device, layout, nullptr);
	}

	for (auto *buffer : {&vertex_buffer, &index_buffer, &scene_buffer, &model_buffer,
						 &point_buffer, &spot_buffer}) {
		if (buffer->buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(context.allocator, buffer->buffer,
							 buffer->allocation);
		}
	}
}

void update(double time) {
	// Animation time
	double dt = 0.0;
	if (last_time > 0.0) {
		dt = time - last_time;
	}
	last_time = time;

	if (anim_playing) {
		anim_time += static_cast<float>(dt) * anim_speed;
	}
	texture_time = static_cast<float>(time);

	// ImGui
	ImGui::Begin("Regular Octahedron");

	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::DragFloat3("Position", glm::value_ptr(camera_pos), 0.05f);
		ImGui::SliderFloat("Yaw", &camera_yaw, -180.0f, 180.0f);
		ImGui::SliderFloat("Pitch", &camera_pitch, -89.0f, 89.0f, "%.1f",
						   ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::Button("Reset camera")) {
			camera_pos = {0.0f, 2.5f, 6.0f};
			camera_yaw = 0.0f;
			camera_pitch = -22.62f;
		}
		const char *proj_items[] = {"Perspective", "Orthographic"};
		ImGui::Combo("Projection mode", &current_proj_item, proj_items, 2);
		if (current_proj_item == 0) {
			ImGui::SliderFloat("Field of view (FOV)", &fov, 20.0f, 110.0f,
							   "%.1f", ImGuiSliderFlags_AlwaysClamp);
		} else {
			ImGui::SliderFloat("Orthographic size", &ortho_size, 0.5f, 10.0f,
							   "%.2f", ImGuiSliderFlags_AlwaysClamp);
		}
	}

	if (ImGui::CollapsingHeader("Object transforms")) {
		ImGui::DragFloat3("Position (X, Y, Z)", glm::value_ptr(obj_pos), 0.05f);
		ImGui::SliderFloat3("Rotation (Pitch, Yaw, Roll)",
							glm::value_ptr(obj_rot), -180.0f, 180.0f);
		ImGui::DragFloat3("Scale", glm::value_ptr(obj_scale), 0.05f, 0.1f,
						  10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::Button("Reset transforms")) {
			obj_pos = {0.0f, 0.0f, 0.0f};
			obj_rot = {0.0f, 0.0f, 0.0f};
			obj_scale = {1.0f, 1.0f, 1.0f};
		}
	}

	if (ImGui::CollapsingHeader("Path animation")) {
		ImGui::Checkbox("Enable path", &enable_trajectory);
		ImGui::SameLine();
		if (ImGui::Button(anim_playing ? " Pause " : "Start")) {
			anim_playing = !anim_playing;
		}
		ImGui::SliderFloat("Animation speed", &anim_speed, 0.0f, 5.0f);
		ImGui::SliderFloat("Path radius", &trajectory_radius, 0.5f, 8.0f);
	}

	if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Combo("Main texture", &selected_textures[0], texture_names, texture_count);
		ImGui::Combo("Second texture", &selected_textures[1], texture_names, texture_count);
		ImGui::Checkbox("Animate main texture", &animated_texture);
		ImGui::ColorEdit3("Specular color", glm::value_ptr(specular_color));
		ImGui::SliderFloat("Shininess", &shininess, 1.0f, 128.0f, "%.1f",
						   ImGuiSliderFlags_AlwaysClamp);
	}

	if (ImGui::CollapsingHeader("Ambient light")) {
		ImGui::PushID("ambient");
		editLightColor(ambient);
		ImGui::PopID();
	}
	if (ImGui::CollapsingHeader("Directional light")) {
		ImGui::PushID("directional");
		ImGui::DragFloat3("Direction", glm::value_ptr(light_direction), 0.05f);
		editLightColor(light_color);
		ImGui::PopID();
	}

	if (ImGui::CollapsingHeader("Point lights")) {
		ImGui::BeginDisabled(point_lights.size() >= max_lights);
		if (ImGui::Button("Add point light")) {
			point_lights.emplace_back();
		}
		ImGui::EndDisabled();

		for (size_t i = 0; i < point_lights.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			bool remove = false;
			if (ImGui::TreeNode("point", "Point %zu", i + 1)) {
				editPointLight(point_lights[i]);
				remove = ImGui::Button("Remove");
				ImGui::TreePop();
			}
			ImGui::PopID();
			if (remove) {
				point_lights.erase(point_lights.begin() + i);
				break;
			}
		}
	}

	if (ImGui::CollapsingHeader("Spotlights")) {
		ImGui::BeginDisabled(spot_lights.size() >= max_lights);
		if (ImGui::Button("Add spotlight")) {
			spot_lights.emplace_back();
		}
		ImGui::EndDisabled();

		for (size_t i = 0; i < spot_lights.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			bool remove = false;
			if (ImGui::TreeNode("spot", "Spot %zu", i + 1)) {
				auto &spot = spot_lights[i];
				editPointLight(spot.light);
				ImGui::DragFloat3("Direction", glm::value_ptr(spot.direction),
								  0.05f);
				ImGui::SliderFloat("Inner angle", &spot.angles.x, 0.0f,
								   spot.angles.y, "%.1f",
								   ImGuiSliderFlags_AlwaysClamp);
				ImGui::SliderFloat("Outer angle", &spot.angles.y, spot.angles.x,
								   80.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
				remove = ImGui::Button("Remove");
				ImGui::TreePop();
			}
			ImGui::PopID();
			if (remove) {
				spot_lights.erase(spot_lights.begin() + i);
				break;
			}
		}
	}

	ImGui::End();

	// View and projection
	const auto extent = graphics::internal::context.swapchain_extent;
	float aspect =
		static_cast<float>(extent.width) / static_cast<float>(extent.height);
	if (aspect <= 0.0f) {
		aspect = 1.0f;
	}

	const float yaw = glm::radians(camera_yaw);
	const float pitch = glm::radians(camera_pitch);
	const glm::vec3 forward{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
							-std::cos(yaw) * std::cos(pitch)};
	const glm::mat4 view = glm::lookAt(camera_pos, camera_pos + forward,
									   glm::vec3(0.0f, 1.0f, 0.0f));

	glm::mat4 proj;
	if (current_proj_item == 0) {
		proj = glm::perspective(glm::radians(fov), aspect, 0.1f, 100.0f);
	} else {
		proj = glm::ortho(-ortho_size * aspect, ortho_size * aspect,
						  -ortho_size, ortho_size, 0.1f, 100.0f);
	}
	proj[1][1] *= -1.0f; // Invert Y

	// Model matrix
	glm::vec3 cur_pos = obj_pos;
	glm::vec3 cur_rot = obj_rot;

	if (enable_trajectory) {
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

	// Shader data
	const SceneData scene = {
		.view_projection = proj * view,
		.camera = current_proj_item == 0 ? glm::vec4(camera_pos, 1.0f)
										 : glm::vec4(-forward, 0.0f),
		.ambient = ambient,
		.light_direction = light_direction,
		.light_color = light_color,
		.light_counts = {static_cast<int>(point_lights.size()),
						 static_cast<int>(spot_lights.size()), 0, 0},
	};
	const ModelData object = {
		.model = model,
		.normal_matrix = glm::transpose(glm::inverse(model)),
		.specular = glm::vec4(specular_color, shininess),
	};
	const auto second_transform = glm::translate(glm::mat4(1.0f), glm::vec3(2.2f, -0.2f, 0.0f));
	const auto second_model = glm::scale(second_transform, glm::vec3(0.7f));
	const ModelData second_object = {
		.model = second_model,
		.normal_matrix = glm::transpose(glm::inverse(second_model)),
		.specular = object.specular,
	};
	writeBuffer(scene_buffer, &scene, sizeof(scene));
	writeBuffer(model_buffer, &object, sizeof(object));
	writeBuffer(model_buffer, &second_object, sizeof(second_object), model_stride);
	writeBuffer(point_buffer, point_lights.data(),
				sizeof(PointLight) * point_lights.size());
	writeBuffer(spot_buffer, spot_lights.data(),
				sizeof(SpotLight) * spot_lights.size());
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
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer.buffer,
						   offsets);
	vkCmdBindIndexBuffer(fd.command_buffer, index_buffer.buffer, 0,
						 VK_INDEX_TYPE_UINT16);

	// Objects
	vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
							pipeline_layout, 0, 1, descriptor_sets, 0, nullptr);
	for (uint32_t i = 0; i < object_count; ++i) {
		const uint32_t offset = i * model_stride;
		const auto material = descriptor_sets[1 + selected_textures[i]];
		const TextureEffect effect{texture_time, i == 0 && animated_texture};
		vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
								pipeline_layout, 1, 1, &material, 1, &offset);
		vkCmdPushConstants(fd.command_buffer, pipeline_layout,
						   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(effect), &effect);
		vkCmdDrawIndexed(fd.command_buffer, octahedron_count, 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(fd.command_buffer);

	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
