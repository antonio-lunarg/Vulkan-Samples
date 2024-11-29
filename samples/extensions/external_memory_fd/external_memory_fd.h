/* Copyright (c) 2024, LunarG
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

class ExternalMemoryFD : public vkb::VulkanSampleCpp
{
  public:
	ExternalMemoryFD();

	virtual bool prepare(const vkb::ApplicationOptions &options) override;

	virtual void update(float delta_time) override;

	void draw(vkb::core::HPPCommandBuffer &command_buffer, vkb::rendering::HPPRenderTarget &render_target);

	/// @brief Copy the render target color image ready for presentation to an exportable image
	void copy_color_image_to_exportable_image(
		vkb::core::HPPCommandBuffer &command_buffer,
		vkb::rendering::HPPRenderTarget &render_target
	);

	virtual ~ExternalMemoryFD() override;

  private:
	/// @brief Create a VMA memory pool for images with exportable memory
	void create_memory_pool();

	/// @brief Create the image with exportable memory
	void create_exportable_image();

	/// @brief Export image memory to a file descriptor
	void export_memory();

	/// Memory pool for exportable memory
	std::unique_ptr<VmaPool> pool;

	/// Image created with an exportable memory allocation
	std::unique_ptr<vkb::core::HPPImage>     exportable_image;
	std::unique_ptr<vkb::core::HPPImageView>     exportable_image_view;

	int fd = -1;
};

std::unique_ptr<vkb::VulkanSampleCpp> create_external_memory_fd();
