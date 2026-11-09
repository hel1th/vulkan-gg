#include "upload.hpp"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_WINDOWS_UTF8
#include <stb_image.h>

namespace application {

namespace {

// Transfer commands
bool submitUpload(auto record) {
	const auto &context = graphics::internal::context;
	const VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = context.graphics_queue_index,
	};
	VkCommandPool pool = VK_NULL_HANDLE;
	if (vkCreateCommandPool(context.device, &pool_info, nullptr, &pool) !=
		VK_SUCCESS) {
		return false;
	}

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	const VkCommandBufferAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	const VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	bool ok = vkAllocateCommandBuffers(context.device, &alloc_info, &cmd) ==
				  VK_SUCCESS &&
			  vkBeginCommandBuffer(cmd, &begin_info) == VK_SUCCESS;
	if (ok) {
		record(cmd);
		const VkSubmitInfo submit_info = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
		};
		ok = vkEndCommandBuffer(cmd) == VK_SUCCESS &&
			 vkQueueSubmit(context.graphics_queue, 1, &submit_info,
						   VK_NULL_HANDLE) == VK_SUCCESS &&
			 vkQueueWaitIdle(context.graphics_queue) == VK_SUCCESS;
	}
	vkDestroyCommandPool(context.device, pool, nullptr);
	return ok;
}

} // namespace

bool createBuffer(DataBuffer &data, VkDeviceSize size, VkBufferUsageFlags usage,
				  bool host_visible) {
	const auto allocator = graphics::internal::context.allocator;
	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VmaAllocationCreateInfo alloc_info = {
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	};
	if (host_visible) {
		alloc_info.flags =
			VMA_ALLOCATION_CREATE_MAPPED_BIT |
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
		alloc_info.requiredFlags = 0;
	}
	VmaAllocationInfo mapped_info{};
	if (vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &data.buffer,
						&data.allocation, &mapped_info) != VK_SUCCESS) {
		return false;
	}
	data.mapped = mapped_info.pMappedData;
	return true;
}

void writeBuffer(const DataBuffer &buffer, const void *data, size_t size,
				 VkDeviceSize offset) {
	if (size == 0) {
		return;
	}
	std::memcpy(static_cast<char *>(buffer.mapped) + offset, data, size);
	vmaFlushAllocation(graphics::internal::context.allocator, buffer.allocation,
					   offset, size);
}

// Geometry
bool uploadBuffer(DataBuffer &buffer, const void *data, VkDeviceSize size,
				  VkBufferUsageFlags usage) {
	if (!createBuffer(buffer, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					  false)) {
		return false;
	}
	DataBuffer staging;
	if (!createBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
		return false;
	}
	writeBuffer(staging, data, size);

	const bool ok = submitUpload([&](VkCommandBuffer cmd) {
		const VkBufferCopy copy = {.size = size};
		vkCmdCopyBuffer(cmd, staging.buffer, buffer.buffer, 1, &copy);
		const VkAccessFlags read_access =
			usage == VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
				? VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
				: VK_ACCESS_INDEX_READ_BIT;
		const VkBufferMemoryBarrier barrier = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = read_access,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = buffer.buffer,
			.size = size,
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr,
							 1, &barrier, 0, nullptr);
	});
	vmaDestroyBuffer(graphics::internal::context.allocator, staging.buffer,
					 staging.allocation);
	return ok;
}

// Texture
bool loadTexture(Texture &texture, const char *base_path) {
	const std::filesystem::path base(
		std::u8string(base_path, base_path + std::strlen(base_path)));
	auto path = base;
	for (const char *extension : {".png", ".jpg", ".jpeg"}) {
		path = base;
		path += extension;
		std::error_code error;
		if (std::filesystem::is_regular_file(path, error)) {
			break;
		}
	}
	int width = 0, height = 0;
	const auto filename = path.u8string();
	std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
		stbi_load(reinterpret_cast<const char *>(filename.c_str()), &width,
				  &height, nullptr, STBI_rgb_alpha),
		stbi_image_free);
	const stbi_uc blank_pixel[] = {255, 255, 255, 255};
	if (!decoded) {
		std::cerr << "Using blank texture for: " << base_path << '\n';
		width = height = 1;
	}

	const auto &context = graphics::internal::context;
	const auto size = static_cast<size_t>(width) * height * STBI_rgb_alpha;
	const VkExtent3D extent{static_cast<uint32_t>(width),
							static_cast<uint32_t>(height), 1};
	DataBuffer staging;
	if (!createBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
		return false;
	}
	writeBuffer(staging, decoded ? decoded.get() : blank_pixel, size);
	decoded.reset();

	constexpr VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
	const VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = extent,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	const VmaAllocationCreateInfo alloc_info = {
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	};
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	const bool ok =
		vmaCreateImage(context.allocator, &image_info, &alloc_info,
					   &texture.image, &texture.allocation,
					   nullptr) == VK_SUCCESS &&
		submitUpload([&](VkCommandBuffer cmd) {
			VkImageMemoryBarrier barrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = texture.image,
				.subresourceRange = range,
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
								 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
								 0, nullptr, 1, &barrier);
			const VkBufferImageCopy copy = {
				.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				.imageExtent = extent,
			};
			vkCmdCopyBufferToImage(cmd, staging.buffer, texture.image,
								   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
								   &copy);
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
								 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
								 nullptr, 0, nullptr, 1, &barrier);
		});
	vmaDestroyBuffer(context.allocator, staging.buffer, staging.allocation);
	if (!ok) {
		return false;
	}

	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = texture.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = range,
	};
	return vkCreateImageView(context.device, &view_info, nullptr,
							 &texture.view) == VK_SUCCESS;
}

} // namespace application
