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

#include "common/vk_common.h"
#include "core/allocated.h"
#include "core/image.h"
#include "external_memory_fd.h"
#include "filesystem/legacy.h"
#include "gltf_loader.h"
#include "gui.h"
#include "platform/platform.h"
#include "rendering/subpasses/forward_subpass.h"
#include "stats/stats.h"

ExternalMemoryFD::ExternalMemoryFD()
{
	add_device_extension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
	add_device_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
	add_instance_extension(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
}

ExternalMemoryFD::~ExternalMemoryFD()
{
	exportable_image.reset();
	vmaDestroyPool(vkb::allocated::get_memory_allocator(), *pool);
}

bool ExternalMemoryFD::prepare(const vkb::ApplicationOptions &options)
{
	if (!VulkanSample::prepare(options))
	{
		return false;
	}

	// Load a scene from the assets folder
	load_scene("scenes/sponza/Sponza01.gltf");

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

	create_exportable_image();
	export_memory();

	return true;
}

void ExternalMemoryFD::create_exportable_image()
{
	assert(exportable_image == nullptr);

	create_memory_pool();

	auto extent = get_render_context().get_surface_extent();
	exportable_image =
	    vkb::core::HPPImageBuilder(extent.width, extent.height)
	        .with_vma_pool(*pool)
	        .with_usage(vk::ImageUsageFlagBits::eTransferDst)
	        .with_vma_required_flags(vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eHostVisible)
	        .with_tiling(vk::ImageTiling::eLinear)
	        .with_format(get_render_context().get_format())
	        .build_unique(get_device());

	exportable_image_view = std::make_unique<vkb::core::HPPImageView>(*exportable_image, vk::ImageViewType::e2D);
}

void ExternalMemoryFD::create_memory_pool()
{
	assert(pool == nullptr);

	// Define an example buffer and allocation parameters.
	VkExternalMemoryBufferCreateInfoKHR external_mem_buf_create_info = {
	    VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO_KHR,
	    nullptr,
	    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
	VkBufferCreateInfo example_buf_create_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	example_buf_create_info.size               = 0x10000;        // Doesn't matter here.
	example_buf_create_info.usage              = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	example_buf_create_info.pNext              = &external_mem_buf_create_info;

	VmaAllocationCreateInfo example_alloc_create_info = {};
	example_alloc_create_info.usage                   = VMA_MEMORY_USAGE_AUTO;

	// Find memory type index to use for the custom pool.
	uint32_t mem_type_index;

	VkResult res = vmaFindMemoryTypeIndexForBufferInfo(vkb::allocated::get_memory_allocator(),
	                                                   &example_buf_create_info, &example_alloc_create_info, &mem_type_index);
	VK_CHECK(res);

	// Create a custom pool.
	constexpr static VkExportMemoryAllocateInfoKHR export_mem_alloc_info = {
	    VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR,
	    nullptr,
	    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
	VmaPoolCreateInfo poolCreateInfo   = {};
	poolCreateInfo.memoryTypeIndex     = mem_type_index;
	poolCreateInfo.pMemoryAllocateNext = (void *) &export_mem_alloc_info;

	VmaPool vma_pool;
	res = vmaCreatePool(vkb::allocated::get_memory_allocator(), &poolCreateInfo, &vma_pool);
	VK_CHECK(res);

	pool = std::make_unique<VmaPool>(vma_pool);
}

void ExternalMemoryFD::export_memory()
{
	assert(fd < 0);
	assert(exportable_image != nullptr);

	// To export memory, there is a VkGetMemory* function provided by the given external memory extension.
	// This function will take in a VkDeviceMemory handle and then map that to the extension exposed object.

	VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocate_info.pNext = &export_info;

	VkMemoryGetFdInfoKHR get_handle_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	get_handle_info.memory     = exportable_image->get_memory();
	get_handle_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	VkResult res = vkGetMemoryFdKHR(get_device().get_handle(), &get_handle_info, &fd);
	VK_CHECK(res);

	LOGI("Memory exported to {}", fd);
}

void ExternalMemoryFD::update(float delta_time)
{
	vkb::Application::update(delta_time);

	update_scene(delta_time);

	update_gui(delta_time);

	auto &render_context = get_render_context();

	auto &command_buffer = render_context.begin();

	command_buffer.begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	//  custom draw to inject a copy swapchain to image or to external fd
	draw(command_buffer, render_context.get_active_frame().get_render_target());

	command_buffer.end();

	render_context.submit(command_buffer);
}

void ExternalMemoryFD::draw(vkb::core::HPPCommandBuffer &command_buffer, vkb::rendering::HPPRenderTarget &render_target)
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

	copy_color_image_to_exportable_image(command_buffer, render_target);

	// Prepare target image for presentation
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eTransferSrcOptimal;
		memory_barrier.new_layout      = vk::ImageLayout::ePresentSrcKHR;
		memory_barrier.src_access_mask = vk::AccessFlagBits::eTransferWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eTransfer;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eBottomOfPipe;

		command_buffer.image_memory_barrier(views[0], memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);
	}
}

void ExternalMemoryFD::copy_color_image_to_exportable_image(
    vkb::core::HPPCommandBuffer     &command_buffer,
    vkb::rendering::HPPRenderTarget &render_target)
{
	assert(exportable_image != nullptr);

	auto &color_image = render_target.get_images()[0];
	auto &color_view  = render_target.get_views()[0];

	assert(color_image.get_extent() == exportable_image->get_extent());

	// Prepare color image for copy
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eColorAttachmentOptimal;
		memory_barrier.new_layout      = vk::ImageLayout::eTransferSrcOptimal;
		memory_barrier.src_access_mask = vk::AccessFlagBits::eColorAttachmentWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eTransfer;

		command_buffer.image_memory_barrier(color_view, memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);
	}

	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eUndefined;
		memory_barrier.new_layout      = vk::ImageLayout::eTransferDstOptimal;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eTopOfPipe;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eTransfer;

		command_buffer.image_memory_barrier(*exportable_image_view, memory_barrier);
	}

	vk::ImageCopy image_copy = {};
	image_copy.extent        = color_image.get_extent();
	image_copy.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	image_copy.srcSubresource.layerCount = 1;
	image_copy.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	image_copy.dstSubresource.layerCount = 1;

	const std::vector<vk::ImageCopy> regions = {image_copy};

	command_buffer.copy_image(
	    color_image,
	    *exportable_image,
	    regions);
}

std::unique_ptr<vkb::VulkanSampleCpp> create_external_memory_fd()
{
	return std::make_unique<ExternalMemoryFD>();
}
