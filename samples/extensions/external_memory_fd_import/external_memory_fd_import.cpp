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

#include <fcntl.h>
#include <unistd.h>

#include "common/vk_common.h"
#include "core/allocated.h"
#include "external_memory_fd_import.h"
#include "filesystem/legacy.h"
#include "gltf_loader.h"
#include "gui.h"
#include "platform/platform.h"
#include "rendering/subpasses/forward_subpass.h"
#include "stats/stats.h"

ExternalMemoryFDImport::ExternalMemoryFDImport()
{
	add_device_extension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
	add_device_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
	add_instance_extension(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
}

bool ExternalMemoryFDImport::prepare(const vkb::ApplicationOptions &options)
{
	if (!VulkanSample::prepare(options))
	{
		return false;
	}

	// Load a scene from the assets folder
	load_scene("scenes/cube.gltf");

	// Attach a move script to the camera component in the scene
	auto &camera_node = vkb::common::add_free_camera(get_scene(), "main_camera", get_render_context().get_surface_extent());
	auto  camera      = &camera_node.get_component<vkb::sg::Camera>();

	// Example Scene Render Pipeline
	vkb::ShaderSource vert_shader("base.vert");
	vkb::ShaderSource frag_shader("base.frag");
	auto              scene_subpass   = std::make_unique<vkb::rendering::subpasses::HPPForwardSubpass>(get_render_context(), std::move(vert_shader), std::move(frag_shader), get_scene(), *camera);
	auto              render_pipeline = std::make_unique<vkb::rendering::HPPRenderPipeline>();
	render_pipeline->add_subpass(std::move(scene_subpass));
	set_render_pipeline(std::move(render_pipeline));

	// Add a GUI with the stats you want to monitor
	create_gui(*window);

	create_imported_image();

	return true;
}

void ExternalMemoryFDImport::create_imported_image()
{
	assert(imported_image == VK_NULL_HANDLE);
	assert(hpp_imported_image == nullptr);

	const auto extent = vk::Extent3D{
	    get_render_context().get_surface_extent().width,
	    get_render_context().get_surface_extent().height,
	    1};

	// VMA does does not provide a way to create images with imported memory yet
	vk::ImageCreateInfo image_create_info;
	image_create_info.imageType   = vk::ImageType::e2D;
	image_create_info.format      = get_render_context().get_format();
	image_create_info.extent      = extent;
	image_create_info.mipLevels   = 1;
	image_create_info.arrayLayers = 1;
	image_create_info.samples     = vk::SampleCountFlagBits::e1;
	image_create_info.tiling      = vk::ImageTiling::eLinear;
	image_create_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;

	imported_image = get_device().get_handle().createImage(image_create_info);

	import_memory();

	get_device().get_handle().bindImageMemory(imported_image, imported_memory, 0);

	hpp_imported_image = std::make_unique<vkb::core::HPPImage>(
	    get_device(),
	    imported_image,
	    image_create_info.extent,
	    image_create_info.format,
	    image_create_info.usage);

	imported_image_view = std::make_unique<vkb::core::HPPImageView>(*hpp_imported_image, vk::ImageViewType::e2D);
}

void ExternalMemoryFDImport::import_memory()
{
	assert(imported_memory == VK_NULL_HANDLE);
	assert(imported_image != nullptr);
	open_fd();

	vk::MemoryRequirements mem_reqs = get_device().get_handle().getImageMemoryRequirements(imported_image);

	// To import memory, there is a VkImport*Info struct provided by the given external memory extension.This is passed
	// into vkAllocateMemory where Vulkan will now have a VkDeviceMemory handle that maps to the imported memory.

	auto import_info = vk::ImportMemoryFdInfoKHR()
	                       .setHandleType(
	                           vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd)
	                       .setFd(fd);

	auto memory_allocate_info = vk::MemoryAllocateInfo()
	                                .setAllocationSize(mem_reqs.size)
	                                .setMemoryTypeIndex(
	                                    get_device().get_gpu().get_memory_type(
	                                        mem_reqs.memoryTypeBits,
	                                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent))
	                                .setPNext(&import_info);
	imported_memory = get_device().get_handle().allocateMemory(memory_allocate_info);
}

void ExternalMemoryFDImport::update(float delta_time)
{
	vkb::Application::update(delta_time);

	update_scene(delta_time);

	update_gui(delta_time);

	auto &render_context = get_render_context();

	auto &command_buffer = render_context.begin();

	command_buffer.begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	//  custom draw to inject a copy from external image to swapchain color image
	draw(command_buffer, render_context.get_active_frame().get_render_target());

	command_buffer.end();

	render_context.submit(command_buffer);
}

void ExternalMemoryFDImport::draw(vkb::core::HPPCommandBuffer &command_buffer, vkb::rendering::HPPRenderTarget &render_target)
{
	auto &views = render_target.get_views();

	{
		// Image 0 is the swapchain
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eUndefined;
		memory_barrier.new_layout      = vk::ImageLayout::eColorAttachmentOptimal;
		memory_barrier.src_access_mask = {};
		memory_barrier.dst_access_mask = vk::AccessFlagBits::eColorAttachmentWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eColorAttachmentOutput;

		command_buffer.image_memory_barrier(views[0], memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);

		// Skip 1 as it is handled later as a depth-stencil attachment
		for (size_t i = 2; i < views.size(); ++i)
		{
			command_buffer.image_memory_barrier(views[i], memory_barrier);
			render_target.set_layout(static_cast<uint32_t>(i), memory_barrier.new_layout);
		}
	}

	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eUndefined;
		memory_barrier.new_layout      = vk::ImageLayout::eDepthStencilAttachmentOptimal;
		memory_barrier.src_access_mask = {};
		memory_barrier.dst_access_mask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eTopOfPipe;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;

		command_buffer.image_memory_barrier(views[1], memory_barrier);
		render_target.set_layout(1, memory_barrier.new_layout);
	}

	draw_renderpass(command_buffer, render_target);

	copy_imported_image_to_color_image(command_buffer, render_target);

	// Prepare target image for presentation
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eTransferDstOptimal;
		memory_barrier.new_layout      = vk::ImageLayout::ePresentSrcKHR;
		memory_barrier.src_access_mask = vk::AccessFlagBits::eTransferWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eTransfer;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eBottomOfPipe;

		command_buffer.image_memory_barrier(views[0], memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);
	}
}

void ExternalMemoryFDImport::copy_imported_image_to_color_image(
    vkb::core::HPPCommandBuffer     &command_buffer,
    vkb::rendering::HPPRenderTarget &render_target)
{
	assert(hpp_imported_image != nullptr);

	auto &color_image = render_target.get_images()[0];
	auto &color_view  = render_target.get_views()[0];

	assert(color_image.get_extent() == hpp_imported_image->get_extent());

	// Prepare color image for copy
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eColorAttachmentOptimal;
		memory_barrier.new_layout      = vk::ImageLayout::eTransferDstOptimal;
		memory_barrier.src_access_mask = vk::AccessFlagBits::eColorAttachmentWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eTransfer;

		command_buffer.image_memory_barrier(color_view, memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);
	}
	// Prepare imported image for copy
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eUndefined;
		memory_barrier.new_layout      = vk::ImageLayout::eTransferSrcOptimal;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eTopOfPipe;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eTransfer;

		command_buffer.image_memory_barrier(*imported_image_view, memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);
	}

	vk::ImageCopy image_copy = {};
	image_copy.extent        = color_image.get_extent();

	const std::vector<vk::ImageCopy> regions = {image_copy};

	command_buffer.copy_image(
	    *hpp_imported_image,
	    color_image,
	    regions);
}

void ExternalMemoryFDImport::open_fd()
{
	if (fd >= 0)
	{
		return;        // already open
	}

	fd = dup(70);
	return;

	const char *external_memory_file_path = "/tmp/vulkan-samples-external-memory";

	fd = open(external_memory_file_path, O_RDWR | O_CREAT | O_TRUNC);
	if (fd < 0)
	{
		LOGE("Failed to open external memory file: %s", external_memory_file_path);
	}
}

std::unique_ptr<vkb::VulkanSampleCpp> create_external_memory_fd_import()
{
	return std::make_unique<ExternalMemoryFDImport>();
}
