#pragma once

#include <cstddef>

#include "graphics_internal.hpp"

namespace application {

struct DataBuffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	void *mapped = nullptr;
};

struct Texture {
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
};

bool createBuffer(DataBuffer &data, VkDeviceSize size, VkBufferUsageFlags usage,
				  bool host_visible = true);
void writeBuffer(const DataBuffer &buffer, const void *data, size_t size,
				 VkDeviceSize offset = 0);
bool uploadBuffer(DataBuffer &buffer, const void *data, VkDeviceSize size,
				  VkBufferUsageFlags usage);
bool loadTexture(Texture &texture, const char *base_path);

} // namespace application
