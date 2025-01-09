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

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/vk_common.h"
#include "core/allocated.h"
#include "core/image.h"
#include "external_memory_fd_export.h"
#include "filesystem/legacy.h"
#include "gltf_loader.h"
#include "gui.h"
#include "platform/platform.h"
#include "rendering/subpasses/forward_subpass.h"
#include "stats/stats.h"

#ifdef __linux__
#	define HAVE_MSGHDR_MSG_CONTROL
#endif

ExternalMemoryFDExport::ExternalMemoryFDExport()
{
	add_device_extension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
	add_device_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
	add_instance_extension(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
}

ExternalMemoryFDExport::~ExternalMemoryFDExport()
{
	if (exportable_image != VK_NULL_HANDLE)
	{
		get_device().get_handle().destroyImage(exportable_image);
	}
	if (exportable_memory != VK_NULL_HANDLE)
	{
		get_device().get_handle().freeMemory(exportable_memory);
	}
}

bool ExternalMemoryFDExport::prepare(const vkb::ApplicationOptions &options)
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

	return true;
}

vk::Format ExternalMemoryFDExport::get_image_format() const
{
	return get_render_context().get_format();
}

vk::Extent3D ExternalMemoryFDExport::get_image_extent() const
{
	auto extent = get_render_context().get_surface_extent();
	return vk::Extent3D{extent.width, extent.height, 1};
}

VkDeviceSize ExternalMemoryFDExport::get_image_size() const
{
	auto               extent        = get_render_context().get_surface_extent();
	constexpr uint32_t channel_count = 4;
	constexpr uint32_t channel_depth = 1;
	const VkDeviceSize image_size    = extent.width * extent.height * channel_count * channel_depth;
	return image_size;
}

void ExternalMemoryFDExport::create_exportable_image()
{
	assert(exportable_image == VK_NULL_HANDLE);
	assert(hpp_exportable_image == nullptr);
	assert(hpp_exportable_image_view == nullptr);

	// VMA does not support VK_KHR_external_memory_fd yet
	vk::ExternalMemoryImageCreateInfo external_mem_img_create_info;
	external_mem_img_create_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;

	vk::ImageCreateInfo image_create_info;
	image_create_info.imageType     = vk::ImageType::e2D;
	image_create_info.extent        = get_image_extent();
	image_create_info.format        = get_image_format();
	image_create_info.usage         = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	image_create_info.initialLayout = vk::ImageLayout::eUndefined;
	image_create_info.tiling        = vk::ImageTiling::eLinear;
	image_create_info.samples       = vk::SampleCountFlagBits::e1;
	image_create_info.mipLevels     = 1;
	image_create_info.arrayLayers   = 1;
	image_create_info.setPNext(&external_mem_img_create_info);

	exportable_image = get_device().get_handle().createImage(image_create_info);

	hpp_exportable_image = std::make_unique<vkb::core::HPPImage>(
	    get_device(),
	    exportable_image,
	    image_create_info.extent,
	    image_create_info.format,
	    image_create_info.usage,
	    image_create_info.samples);

	create_exportable_memory();

	get_device().get_handle().bindImageMemory(exportable_image, exportable_memory, 0);

	hpp_exportable_image_view = std::make_unique<vkb::core::HPPImageView>(
	    *hpp_exportable_image, vk::ImageViewType::e2D, image_create_info.format, 0, 0, image_create_info.mipLevels,
	    image_create_info.arrayLayers);
}

void ExternalMemoryFDExport::create_exportable_memory()
{
	assert(exportable_memory == VK_NULL_HANDLE);

	vk::MemoryRequirements mem_reqs = get_device().get_handle().getImageMemoryRequirements(exportable_image);

	vk::ExportMemoryAllocateInfo export_info;
	export_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;

	vk::MemoryAllocateInfo allocate_info;

	allocate_info.allocationSize = get_image_size();

	allocate_info.memoryTypeIndex =
	    get_device().get_gpu().get_memory_type(
	        mem_reqs.memoryTypeBits,
	        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	allocate_info.pNext = &export_info;

	vk::Result result = get_device().get_handle().allocateMemory(&allocate_info, nullptr, &exportable_memory);
	assert(result == vk::Result::eSuccess);
}

void ExternalMemoryFDExport::export_memory()
{
	assert(exportable_image != nullptr);

	vk::MemoryGetFdInfoKHR get_handle_info = {};
	get_handle_info.memory                 = exportable_memory;
	get_handle_info.handleType             = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;

	int exportable_fd = get_device().get_handle().getMemoryFdKHR(get_handle_info);

	LOGI("Memory exported to {}", exportable_fd);

	send_exportable_fd(exportable_fd);
}

void ExternalMemoryFDExport::send_exportable_fd(int exportable_fd)
{
	// Need to send the fd to the importer process
	int external_socket = socket(PF_UNIX, SOCK_STREAM, 0);
	if (external_socket < 0)
	{
		LOGE("Failed to create socket");
		assert(false);
	}
	sockaddr_un un = {};
	un.sun_family  = AF_UNIX;

	const char *socket_name = "/tmp/.external-memory";
	snprintf(un.sun_path, sizeof(un.sun_path), "%s", socket_name);
	unlink(un.sun_path);

	int bind_result = bind(external_socket, (struct sockaddr *) &un, sizeof(un));
	if (bind_result < 0)
	{
		LOGE("Failed to bind socket");
		assert(false);
	}

	int listen_result = listen(external_socket, 1);
	if (listen_result < 0)
	{
		LOGE("Failed to listen on socket");
		assert(false);
	}

	LOGI("Waiting for importer to connect");
	// Blocking
	int conn_fd = accept(external_socket, nullptr, nullptr);
	if (conn_fd < 0)
	{
		LOGE("Failed to accept connection");
		assert(false);
	}

	// Send fd
	ssize_t send_result = send_fd(conn_fd, exportable_fd);
	if (send_result < 0)
	{
		LOGE("Failed to send fd");
		assert(false);
	}

	::close(conn_fd);
	::close(external_socket);
}

int ExternalMemoryFDExport::send_fd(int conn_fd, int exportable_fd)
{
	struct msghdr msg;
	struct iovec  iov[1];

#ifdef HAVE_MSGHDR_MSG_CONTROL
	union
	{
		struct cmsghdr cm;
		char           control[CMSG_SPACE(sizeof(int))];
	} control_un;
	struct cmsghdr *cmptr;

	msg.msg_control    = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);

	cmptr                       = CMSG_FIRSTHDR(&msg);
	cmptr->cmsg_len             = CMSG_LEN(sizeof(int));
	cmptr->cmsg_level           = SOL_SOCKET;
	cmptr->cmsg_type            = SCM_RIGHTS;
	*((int *) CMSG_DATA(cmptr)) = exportable_fd;
#else
	msg.msg_accrights    = (caddr_t) &exportable_fd;
	msg.msg_accrightslen = sizeof(int);
#endif

	msg.msg_name    = NULL;
	msg.msg_namelen = 0;

	const char *c   = "1";
	iov[0].iov_base = (void *) c;
	iov[0].iov_len  = 1;
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;

	return sendmsg(conn_fd, &msg, 0);
}

void ExternalMemoryFDExport::update(float delta_time)
{
	vkb::Application::update(delta_time);

	update_scene(delta_time);

	update_gui(delta_time);

	auto &render_context = get_render_context();

	auto &command_buffer = render_context.begin();

	command_buffer.begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	// custom draw to inject a copy swapchain to image or to external fd
	draw(command_buffer, render_context.get_active_frame().get_render_target());

	command_buffer.end();

	render_context.submit(command_buffer);

	static bool exported = false;
	if (!exported)
	{
		get_device().get_handle().waitIdle();
		export_memory();
		exported = true;
	}
}

void ExternalMemoryFDExport::draw(vkb::core::HPPCommandBuffer &command_buffer, vkb::rendering::HPPRenderTarget &render_target)
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

void ExternalMemoryFDExport::copy_color_image_to_exportable_image(
    vkb::core::HPPCommandBuffer     &command_buffer,
    vkb::rendering::HPPRenderTarget &render_target)
{
	assert(exportable_image != VK_NULL_HANDLE);
	assert(hpp_exportable_image != nullptr);
	assert(hpp_exportable_image_view != nullptr);

	auto &color_image = render_target.get_images()[0];
	auto &color_view  = render_target.get_views()[0];

	// Prepare color image for copy
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eColorAttachmentOptimal;
		memory_barrier.new_layout      = vk::ImageLayout::eTransferSrcOptimal;
		memory_barrier.src_access_mask = vk::AccessFlagBits::eColorAttachmentWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		memory_barrier.dst_access_mask = vk::AccessFlagBits::eTransferRead;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eTransfer;

		command_buffer.image_memory_barrier(color_view, memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);
	}

	// Prepare exportable image for copy
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eUndefined;
		memory_barrier.new_layout      = vk::ImageLayout::eTransferDstOptimal;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eTopOfPipe;
		memory_barrier.dst_access_mask = vk::AccessFlagBits::eTransferWrite;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eTransfer;

		command_buffer.image_memory_barrier(*hpp_exportable_image_view, memory_barrier);
	}

	vk::ImageCopy image_copy             = {};
	image_copy.extent                    = color_image.get_extent();
	image_copy.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	image_copy.srcSubresource.layerCount = 1;
	image_copy.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	image_copy.dstSubresource.layerCount = 1;

	const std::vector<vk::ImageCopy> regions = {image_copy};

	command_buffer.copy_image(
	    color_image,
	    *hpp_exportable_image,
	    regions);
}

std::unique_ptr<vkb::VulkanSampleCpp> create_external_memory_fd_export()
{
	return std::make_unique<ExternalMemoryFDExport>();
}
