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

	/// @brief Copy the imported buffer to the render target color image for presentation
	void copy_imported_buffer_to_color_image(
	    vkb::core::HPPCommandBuffer     &command_buffer,
	    vkb::rendering::HPPRenderTarget &render_target);

  private:
	/// @brief Create the buffer with imported memory
	void create_imported_buffer();

	/// @brief Import memory from a file descriptor
	void import_memory();

	/// @brief Receives the file descriptor for importable memory
	int receive_importable_fd();

	/// @return The file descriptor via UNIX socket
	int receive_fd(int fd);

	/// Imported external memory
	vk::DeviceMemory imported_memory = VK_NULL_HANDLE;

	/// Buffer created with imported memory
	vk::Buffer imported_buffer = VK_NULL_HANDLE;
	std::unique_ptr<vkb::core::BufferCpp> hpp_imported_buffer;
};

std::unique_ptr<vkb::VulkanSampleCpp> create_external_memory_fd_import();
