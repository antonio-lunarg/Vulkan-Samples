/* Copyright (c) 2024, LunarG, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "rendering/render_pipeline.h"
#include "scene_graph/components/camera.h"
#include "vulkan_sample.h"

class ExternalMemoryFDExport : public vkb::VulkanSampleCpp
{
  public:
	ExternalMemoryFDExport();

	~ExternalMemoryFDExport() override;

	virtual bool prepare(const vkb::ApplicationOptions &options) override;

	virtual void update(float delta_time) override;

	void draw(vkb::core::HPPCommandBuffer &command_buffer, vkb::rendering::HPPRenderTarget &render_target);

	/// @brief Copy the render target color image ready for presentation to an exportable image
	void copy_color_image_to_exportable_image(
	    vkb::core::HPPCommandBuffer     &command_buffer,
	    vkb::rendering::HPPRenderTarget &render_target);

  private:
	/// @return The format of the exportable image
	vk::Format get_image_format() const;

	/// @return The extent of the exportable image
	vk::Extent3D get_image_extent() const;

	/// @return The size in bytes of the exportable image
	VkDeviceSize get_image_size() const;

	/// @brief Create the exportable memory
	void create_exportable_memory();

	/// @brief Create the buffer with exportable image
	void create_exportable_image();

	/// @brief Export memory to a file descriptor
	void export_memory();

	/// @brief Sends the memory file descriptor to the importer
	void send_exportable_fd(int exportable_fd);

	/// @brief Sends the file descriptor over UNIX socket
	int send_fd(int conn_fd, int exportable_fd);

	/// Memory which can be exported to an opaque FD
	vk::DeviceMemory exportable_memory = VK_NULL_HANDLE;

	/// Image created with an exportable memory allocation
	vk::Image                                exportable_image = VK_NULL_HANDLE;
	std::unique_ptr<vkb::core::HPPImage>     hpp_exportable_image;
	std::unique_ptr<vkb::core::HPPImageView> hpp_exportable_image_view;
};

std::unique_ptr<vkb::VulkanSampleCpp> create_external_memory_fd_export();
