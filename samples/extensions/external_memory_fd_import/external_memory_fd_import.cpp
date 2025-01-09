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
#include "external_memory_fd_import.h"
#include "filesystem/legacy.h"
#include "gltf_loader.h"
#include "gui.h"
#include "platform/platform.h"
#include "rendering/subpasses/forward_subpass.h"
#include "stats/stats.h"

#ifdef __linux__
#	define HAVE_MSGHDR_MSG_CONTROL
#endif

ExternalMemoryFDImport::ExternalMemoryFDImport()
{
	add_device_extension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
	add_device_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
	add_instance_extension(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
}

ExternalMemoryFDImport::~ExternalMemoryFDImport()
{
	if (imported_image != VK_NULL_HANDLE)
	{
		get_device().get_handle().destroyImage(imported_image);
	}
	if (imported_memory != VK_NULL_HANDLE)
	{
		get_device().get_handle().freeMemory(imported_memory);
	}
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

vk::Format ExternalMemoryFDImport::get_image_format() const
{
	return get_render_context().get_format();
}

vk::Extent3D ExternalMemoryFDImport::get_image_extent() const
{
	auto extent = get_render_context().get_surface_extent();
	return vk::Extent3D{extent.width, extent.height, 1};
}

VkDeviceSize ExternalMemoryFDImport::get_image_size() const
{
	auto               extent        = get_render_context().get_surface_extent();
	constexpr uint32_t channel_count = 4;
	constexpr uint32_t channel_depth = 1;
	const VkDeviceSize image_size    = extent.width * extent.height * channel_count * channel_depth;
	return image_size;
}

void ExternalMemoryFDImport::create_imported_image()
{
	assert(imported_image == VK_NULL_HANDLE);
	assert(hpp_imported_image == nullptr);
	assert(hpp_imported_image_view == nullptr);

	auto               extent        = get_render_context().get_surface_extent();
	constexpr uint32_t channel_count = 4;
	constexpr uint32_t channel_depth = 1;
	const size_t       size          = extent.width * extent.height * channel_count * channel_depth;

	// VMA does does not provide a way to create images with imported memory yet
	vk::ExternalMemoryImageCreateInfo external_mem_img_create_info;
	external_mem_img_create_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;

	vk::ImageCreateInfo image_create_info;
	image_create_info.imageType     = vk::ImageType::e2D;
	image_create_info.extent        = get_image_extent();
	image_create_info.format        = get_image_format();
	image_create_info.usage         = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
	image_create_info.initialLayout = vk::ImageLayout::eUndefined;
	image_create_info.tiling        = vk::ImageTiling::eLinear;
	image_create_info.samples       = vk::SampleCountFlagBits::e1;
	image_create_info.mipLevels     = 1;
	image_create_info.arrayLayers   = 1;
	image_create_info.setPNext(&external_mem_img_create_info);

	imported_image = get_device().get_handle().createImage(image_create_info);

	import_memory();

	get_device().get_handle().bindImageMemory(imported_image, imported_memory, 0);

	hpp_imported_image = std::make_unique<vkb::core::HPPImage>(
	    get_device(),
	    imported_image,
	    image_create_info.extent,
	    image_create_info.format,
	    image_create_info.usage,
	    image_create_info.samples);

	hpp_imported_image_view = std::make_unique<vkb::core::HPPImageView>(
	    *hpp_imported_image, vk::ImageViewType::e2D, image_create_info.format, 0, 0, image_create_info.mipLevels,
	    image_create_info.arrayLayers);
}

void ExternalMemoryFDImport::import_memory()
{
	assert(imported_memory == VK_NULL_HANDLE);
	assert(imported_image != nullptr);

	int imported_fd = receive_importable_fd();

	vk::MemoryRequirements mem_reqs = get_device().get_handle().getImageMemoryRequirements(imported_image);

	// To import memory, there is a VkImport*Info struct provided by the given external memory extension.This is passed
	// into vkAllocateMemory where Vulkan will now have a VkDeviceMemory handle that maps to the imported memory.

	auto import_info = vk::ImportMemoryFdInfoKHR()
	                       .setHandleType(
	                           vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd)
	                       .setFd(imported_fd);

	auto memory_allocate_info = vk::MemoryAllocateInfo()
	                                .setAllocationSize(mem_reqs.size)
	                                .setMemoryTypeIndex(
	                                    get_device().get_gpu().get_memory_type(
	                                        mem_reqs.memoryTypeBits,
	                                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent))
	                                .setPNext(&import_info);
	imported_memory = get_device().get_handle().allocateMemory(memory_allocate_info);
}

int ExternalMemoryFDImport::receive_importable_fd()
{
	int external_socket = socket(PF_UNIX, SOCK_STREAM, 0);
	if (external_socket < 0)
	{
		LOGE("Failed to create socket");
		assert(false);
	}
	sockaddr_un un          = {};
	un.sun_family           = AF_UNIX;
	const char *socket_name = "/tmp/.external-memory";
	snprintf(un.sun_path, sizeof(un.sun_path), "%s", socket_name);

	int connect_result = connect(external_socket, (struct sockaddr *) &un, sizeof(un));
	if (connect_result < 0)
	{
		LOGE("Failed to connect socket");
		assert(false);
	}

	int imported_fd = receive_fd(external_socket);
	assert(imported_fd >= 0);

	::close(external_socket);

	return imported_fd;
}

int ExternalMemoryFDImport::receive_fd(int socket)
{
	struct msghdr msg;
	struct iovec  iov[1];
	ssize_t       n;
	int           newfd;
	int           imported_fd = -1;

#ifdef HAVE_MSGHDR_MSG_CONTROL
	union
	{
		struct cmsghdr cm;
		char           control[CMSG_SPACE(sizeof(int))];
	} control_un;
	struct cmsghdr *cmptr;

	msg.msg_control    = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);
#else
	msg.msg_accrights    = (caddr_t) &newfd;
	msg.msg_accrightslen = sizeof(int);
#endif

	msg.msg_name    = NULL;
	msg.msg_namelen = 0;

	char c;
	iov[0].iov_base = (void *) &c;
	iov[0].iov_len  = 1;
	msg.msg_iov     = iov;
	msg.msg_iovlen  = 1;

	if ((n = recvmsg(socket, &msg, 0)) <= 0)
	{
		LOGE("Failed to receive message");
		assert(false);
	}

#ifdef HAVE_MSGHDR_MSG_CONTROL
	if ((cmptr = CMSG_FIRSTHDR(&msg)) != NULL &&
	    cmptr->cmsg_len == CMSG_LEN(sizeof(int)))
	{
		if (cmptr->cmsg_level != SOL_SOCKET)
		{
			LOGE("control level != SOL_SOCKET");
			return -1;
		}
		if (cmptr->cmsg_type != SCM_RIGHTS)
		{
			LOGE("control type != SCM_RIGHTS");
			return -1;
		}
		imported_fd = *((int *) CMSG_DATA(cmptr));
	}
	else
	{
		imported_fd = -1; /* descriptor was not passed */
	}
#else
	if (msg.msg_accrightslen == sizeof(int))
	{
		imported_fd = newfd;
	}
	else
	{
		imported_fd = -1; /* descriptor was not passed */
	}
#endif

	return imported_fd;
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
	copy_imported_image_to_color_image(command_buffer, render_target);

	// Prepare target image for presentation
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eTransferDstOptimal;
		memory_barrier.new_layout      = vk::ImageLayout::ePresentSrcKHR;
		memory_barrier.src_access_mask = vk::AccessFlagBits::eTransferWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eTransfer;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eBottomOfPipe;

		command_buffer.image_memory_barrier(render_target.get_views()[0], memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);
	}
}

void ExternalMemoryFDImport::copy_imported_image_to_color_image(
    vkb::core::HPPCommandBuffer     &command_buffer,
    vkb::rendering::HPPRenderTarget &render_target)
{
	assert(imported_image != VK_NULL_HANDLE);
	assert(hpp_imported_image != nullptr);
	assert(hpp_imported_image_view != nullptr);

	auto &color_image = render_target.get_images()[0];
	auto &color_view  = render_target.get_views()[0];

	// Prepare exportable image for copy
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eUndefined;
		memory_barrier.new_layout      = vk::ImageLayout::eTransferSrcOptimal;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eTopOfPipe;
		memory_barrier.dst_access_mask = vk::AccessFlagBits::eTransferRead;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eTransfer;

		command_buffer.image_memory_barrier(*hpp_imported_image_view, memory_barrier);
	}

	// Prepare color image for copy
	{
		vkb::common::HPPImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout      = vk::ImageLayout::eUndefined;
		memory_barrier.new_layout      = vk::ImageLayout::eTransferDstOptimal;
		memory_barrier.src_access_mask = vk::AccessFlagBits::eColorAttachmentWrite;
		memory_barrier.src_stage_mask  = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		memory_barrier.dst_access_mask = vk::AccessFlagBits::eTransferWrite;
		memory_barrier.dst_stage_mask  = vk::PipelineStageFlagBits::eTransfer;

		command_buffer.image_memory_barrier(color_view, memory_barrier);
		render_target.set_layout(0, memory_barrier.new_layout);
	}

	vk::ImageCopy image_copy             = {};
	image_copy.extent                    = color_image.get_extent();
	image_copy.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	image_copy.srcSubresource.layerCount = 1;
	image_copy.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	image_copy.dstSubresource.layerCount = 1;

	const std::vector<vk::ImageCopy> regions = {image_copy};

	command_buffer.copy_image(
	    *hpp_imported_image,
	    color_image,
	    regions);
}

std::unique_ptr<vkb::VulkanSampleCpp> create_external_memory_fd_import()
{
	return std::make_unique<ExternalMemoryFDImport>();
}
