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

class ExternalMemoryFDImport : public vkb::VulkanSampleCpp
{
  public:
	ExternalMemoryFDImport();

	~ExternalMemoryFDImport() override;

	virtual bool prepare(const vkb::ApplicationOptions &options) override;

	virtual void update(float delta_time) override;

	void draw(vkb::core::HPPCommandBuffer &command_buffer, vkb::rendering::HPPRenderTarget &render_target);

	/// @brief Copy the imported image to the render target color image for presentation
	void copy_imported_image_to_color_image(
	    vkb::core::HPPCommandBuffer     &command_buffer,
	    vkb::rendering::HPPRenderTarget &render_target);

  private:
	/// @return The format of the imported image
	vk::Format get_image_format() const;

	/// @return The extent of the imported image
	vk::Extent3D get_image_extent() const;

	/// @return The size in bytes of the imported image
	VkDeviceSize get_image_size() const;

	/// @brief Create the image with imported memory
	void create_imported_image();

	/// @brief Import memory from a file descriptor
	void import_memory();

	/// @brief Receives the file descriptor for importable memory
	int receive_importable_fd();

	/// @return The file descriptor via UNIX socket
	int receive_fd(int fd);

	/// Imported external memory
	vk::DeviceMemory imported_memory = VK_NULL_HANDLE;

	/// Image created with imported memory
	vk::Image                                imported_image = VK_NULL_HANDLE;
	std::unique_ptr<vkb::core::HPPImage>     hpp_imported_image;
	std::unique_ptr<vkb::core::HPPImageView> hpp_imported_image_view;
};

std::unique_ptr<vkb::VulkanSampleCpp> create_external_memory_fd_import();
