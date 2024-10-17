/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file vulkanGraphicsBuffer.cxx
 * @author rdb
 * @date 2016-04-13
 */

#include "vulkanGraphicsBuffer.h"
#include "vulkanGraphicsStateGuardian.h"

TypeHandle VulkanGraphicsBuffer::_type_handle;

/**
 *
 */
VulkanGraphicsBuffer::
VulkanGraphicsBuffer(GraphicsEngine *engine, GraphicsPipe *pipe,
                     const std::string &name,
                     const FrameBufferProperties &fb_prop,
                     const WindowProperties &win_prop,
                     int flags,
                     GraphicsStateGuardian *gsg,
                     GraphicsOutput *host) :
  GraphicsBuffer(engine, pipe, name, fb_prop, win_prop, flags, gsg, host),
  _render_pass(VK_NULL_HANDLE),
  _framebuffer(VK_NULL_HANDLE) {
}

/**
 *
 */
VulkanGraphicsBuffer::
~VulkanGraphicsBuffer() {
}

/**
 * Returns true if this particular GraphicsOutput can render directly into a
 * texture, or false if it must always copy-to-texture at the end of each
 * frame to achieve this effect.
 */
bool VulkanGraphicsBuffer::
get_supports_render_texture() const {
  //TODO: add render-to-texture support.
  return false;
}

/**
 * Clears the entire framebuffer before rendering, according to the settings
 * of get_color_clear_active() and get_depth_clear_active() (inherited from
 * DrawableRegion).
 *
 * This function is called only within the draw thread.
 */
void VulkanGraphicsBuffer::
clear(Thread *current_thread) {
  // We do the clear in begin_frame(), and the validation layers don't like it
  // if an extra clear is being done at the beginning of a frame.  That's why
  // this is empty for now.  Need a cleaner solution for this.
}

/**
 * This function will be called within the draw thread before beginning
 * rendering for a given frame.  It should do whatever setup is required, and
 * return true if the frame should be rendered, or false if it should be
 * skipped.
 */
bool VulkanGraphicsBuffer::
begin_frame(FrameMode mode, Thread *current_thread) {
  begin_frame_spam(mode);
  if (_gsg == nullptr) {
    return false;
  }

  if (mode != FM_render) {
    return true;
  }

  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_R(vkgsg, _gsg, false);

  if (vkgsg->needs_reset()) {
    vkQueueWaitIdle(vkgsg->_queue);
    destroy_framebuffer();
    if (_render_pass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(vkgsg->_device, _render_pass, nullptr);
      _render_pass = VK_NULL_HANDLE;
    }
    vkgsg->reset_if_new();
  }


  if (!vkgsg->is_valid()) {
    return false;
  }

  vkgsg->_fb_color_tc = nullptr;
  vkgsg->_fb_depth_tc = nullptr;

  if (_current_clear_mask != _clear_mask) {
    // The clear flags have changed.  Recreate the render pass.  Note that the
    // clear flags don't factor into render pass compatibility, so we don't
    // need to recreate the framebuffer.
    setup_render_pass();
  }

  if (_creation_flags & GraphicsPipe::BF_size_track_host) {
    if (_host != nullptr) {
      _size = _host->get_size();
    }
  }

  if (_framebuffer_size != _size) {
    // Uh-oh, the window must have resized.  Recreate the framebuffer.
    // Before destroying the old, make sure the queue is no longer rendering
    // anything to it.
    destroy_framebuffer();
    if (!create_framebuffer()) {
      return false;
    }
  }

  // Instruct the GSG that we are commencing a new frame.  This will cause it
  // to create a command buffer.
  vkgsg->set_current_properties(&get_fb_properties());
  if (!vkgsg->begin_frame(current_thread)) {
    return false;
  }

  copy_async_screenshot();

  /*if (mode == FM_render) {
    clear_cube_map_selection();
  }*/

  // Now that we have a command buffer, start our render pass.  First
  // transition the swapchain images into the valid state for rendering into.
  VkCommandBuffer cmd = vkgsg->_frame_data->_cmd;

  VkClearValue *clears = (VkClearValue *)
    alloca(sizeof(VkClearValue) * _attachments.size());

  VkRenderPassBeginInfo begin_info;
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.pNext = nullptr;
  begin_info.renderPass = _render_pass;
  begin_info.framebuffer = _framebuffer;
  begin_info.renderArea.offset.x = 0;
  begin_info.renderArea.offset.y = 0;
  begin_info.renderArea.extent.width = _size[0];
  begin_info.renderArea.extent.height = _size[1];
  begin_info.clearValueCount = _attachments.size();
  begin_info.pClearValues = clears;

  for (size_t i = 0; i < _attachments.size(); ++i) {
    Attachment &attach = _attachments[i];
    attach._tc->set_active(true);

    VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAccessFlags write_access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkAccessFlags read_access_mask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    VkPipelineStageFlags stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    if (attach._plane == RTP_stencil || attach._plane == RTP_depth ||
        attach._plane == RTP_depth_stencil) {
      vkgsg->_fb_depth_tc = attach._tc;
      layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      stage_mask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                 | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      write_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      read_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    else if (attach._plane == RTP_color) {
      vkgsg->_fb_color_tc = attach._tc;
    }

    if (get_clear_active(attach._plane)) {
      LColor clear_value = get_clear_value(attach._plane);
      clears[i].color.float32[0] = clear_value[0];
      clears[i].color.float32[1] = clear_value[1];
      clears[i].color.float32[2] = clear_value[2];
      clears[i].color.float32[3] = clear_value[3];

      // This transition will be made when the first subpass is started.
      attach._tc->_layout = layout;
      attach._tc->_read_stage_mask = stage_mask;
      attach._tc->_write_stage_mask = stage_mask;
      attach._tc->_write_access_mask = write_access_mask;
    } else {
      attach._tc->transition(cmd, vkgsg->_graphics_queue_family_index,
                             layout, stage_mask, read_access_mask | write_access_mask);
    }
  }

  vkCmdBeginRenderPass(cmd, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
  vkgsg->_render_pass = _render_pass;
  vkgsg->_fb_ms_count = VK_SAMPLE_COUNT_1_BIT;

  return true;
}

/**
 * This function will be called within the draw thread after rendering is
 * completed for a given frame.  It should do whatever finalization is
 * required.
 */
void VulkanGraphicsBuffer::
end_frame(FrameMode mode, Thread *current_thread) {
  end_frame_spam(mode);

  if (mode == FM_render) {
    VulkanGraphicsStateGuardian *vkgsg;
    DCAST_INTO_V(vkgsg, _gsg);
    VkCommandBuffer cmd = vkgsg->_frame_data->_cmd;
    nassertv(cmd != VK_NULL_HANDLE);

    vkCmdEndRenderPass(cmd);
    vkgsg->_render_pass = VK_NULL_HANDLE;

    // The driver implicitly transitioned this to the final layout.
    for (Attachment &attach : _attachments) {
      attach._tc->_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

      // This seems to squelch a validation warning, not sure about this yet
      attach._tc->_write_stage_mask |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }

    // Now we can do copy-to-texture, now that the render pass has ended.
    copy_to_textures();

    // Note: this will close the command buffer.
    _gsg->end_frame(current_thread);

    trigger_flip();
    clear_cube_map_selection();
  }
}

/**
 * Closes the buffer right now.  Called from the window thread.
 */
void VulkanGraphicsBuffer::
close_buffer() {
  if (!_gsg.is_null()) {
    VulkanGraphicsStateGuardian *vkgsg;
    DCAST_INTO_V(vkgsg, _gsg);

    destroy_framebuffer();

    if (_render_pass != VK_NULL_HANDLE) {
      if (vkgsg->_last_frame_data != nullptr) {
        vkgsg->_last_frame_data->_pending_destroy_render_passes.push_back(_render_pass);
      } else {
        vkDestroyRenderPass(vkgsg->_device, _render_pass, nullptr);
      }
    }
    _render_pass = VK_NULL_HANDLE;

    _gsg.clear();
  }
}

/**
 * Opens the buffer right now.  Called from the window thread.  Returns true
 * if the buffer is successfully opened, or false if there was a problem.
 */
bool VulkanGraphicsBuffer::
open_buffer() {
  VulkanGraphicsPipe *vkpipe;
  DCAST_INTO_R(vkpipe, _pipe, false);

  // Make sure we have a GSG, which manages a VkDevice.
  VulkanGraphicsStateGuardian *vkgsg;
  uint32_t queue_family_index = 0;
  if (_gsg == nullptr) {
    // Find a queue suitable for graphics.
    if (!vkpipe->find_queue_family(queue_family_index, VK_QUEUE_GRAPHICS_BIT)) {
      vulkandisplay_cat.error()
        << "Failed to find graphics queue for buffer.\n";
      return false;
    }

    // There is no old gsg.  Create a new one.
    vkgsg = new VulkanGraphicsStateGuardian(_engine, vkpipe, nullptr, queue_family_index);
    _gsg = vkgsg;
  } else {
    DCAST_INTO_R(vkgsg, _gsg.p(), false);
  }

  vkgsg->reset_if_new();
  if (!vkgsg->is_valid()) {
    _gsg.clear();
    vulkandisplay_cat.error()
      << "VulkanGraphicsStateGuardian is not valid.\n";
    return false;
  }

  // Choose a suitable color format.  Sorted in order of preferability,
  // preferring lower bpps over higher bpps, and preferring formats that pack
  // bits in fewer channels (because if the user only requests red bits, they
  // probably would prefer to maximize the amount of red bits rather than to
  // have bits in other channels that they don't end up using)
  static const struct {
    int rgb, r, g, b, a;
    bool has_float;
    VkFormat format;
  } formats[] = {
    { 8,  8,  0,  0,  0, false, VK_FORMAT_R8_UNORM},
    {16,  8,  8,  0,  0, false, VK_FORMAT_R8G8_UNORM},
    {16,  5,  6,  5,  0, false, VK_FORMAT_R5G6B5_UNORM_PACK16},
    {15,  5,  5,  5,  1, false, VK_FORMAT_A1R5G5B5_UNORM_PACK16},
    {16, 16,  0,  0,  0,  true, VK_FORMAT_R16_SFLOAT},
    {24,  8,  8,  8,  8, false, VK_FORMAT_B8G8R8A8_UNORM},
    {32, 16, 16,  0,  0, false, VK_FORMAT_R16G16_SFLOAT},
    {30, 10, 10, 10,  2, false, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
    {48, 16, 16, 16, 16,  true, VK_FORMAT_R16G16B16A16_SFLOAT},
    {32, 32,  0,  0,  0,  true, VK_FORMAT_R32_SFLOAT},
    {64, 32, 32,  0,  0,  true, VK_FORMAT_R32G32_SFLOAT},
    {96, 32, 32, 32, 32,  true, VK_FORMAT_R32G32B32A32_SFLOAT},
    {0}
  };

  _color_format = VK_FORMAT_UNDEFINED;

  if (_fb_properties.get_srgb_color()) {
    // This the only sRGB format.  Deal with it.
    _color_format = VK_FORMAT_B8G8R8A8_SRGB;
    _fb_properties.set_rgba_bits(8, 8, 8, 8);
    _fb_properties.set_float_color(false);

  } else if (_fb_properties.get_rgb_color() ||
             _fb_properties.get_color_bits() > 0) {
    for (int i = 0; formats[i].r; ++i) {
      if (formats[i].r >= _fb_properties.get_red_bits() &&
          formats[i].g >= _fb_properties.get_green_bits() &&
          formats[i].b >= _fb_properties.get_blue_bits() &&
          formats[i].a >= _fb_properties.get_alpha_bits() &&
          formats[i].rgb >= _fb_properties.get_color_bits() &&
          formats[i].has_float >= _fb_properties.get_float_color()) {

        // This format meets the requirements.
        _color_format = formats[i].format;
        _fb_properties.set_rgba_bits(formats[i].r, formats[i].g,
                                     formats[i].b, formats[i].a);
        break;
      }
    }
  }

  // Choose a suitable depth/stencil format that satisfies the requirements.
  VkFormatProperties fmt_props;
  bool request_depth32 = _fb_properties.get_depth_bits() > 24 ||
                         _fb_properties.get_float_depth();

  if (_fb_properties.get_depth_bits() > 0 && _fb_properties.get_stencil_bits() > 0) {
    // Vulkan requires support for at least of one of these two formats.
    vkGetPhysicalDeviceFormatProperties(vkpipe->_gpu, VK_FORMAT_D32_SFLOAT_S8_UINT, &fmt_props);
    bool supports_depth32 = (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    vkGetPhysicalDeviceFormatProperties(vkpipe->_gpu, VK_FORMAT_D24_UNORM_S8_UINT, &fmt_props);
    bool supports_depth24 = (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    if ((supports_depth32 && request_depth32) || !supports_depth24) {
      _depth_stencil_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
      _fb_properties.set_depth_bits(32);
    } else {
      _depth_stencil_format = VK_FORMAT_D24_UNORM_S8_UINT;
      _fb_properties.set_depth_bits(24);
    }
    _fb_properties.set_stencil_bits(8);

    _depth_stencil_plane = RTP_depth_stencil;

  } else if (_fb_properties.get_depth_bits() > 0) {
    // Vulkan requires support for at least of one of these two formats.
    vkGetPhysicalDeviceFormatProperties(vkpipe->_gpu, VK_FORMAT_D32_SFLOAT, &fmt_props);
    bool supports_depth32 = (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    vkGetPhysicalDeviceFormatProperties(vkpipe->_gpu, VK_FORMAT_X8_D24_UNORM_PACK32, &fmt_props);
    bool supports_depth24 = (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    if ((supports_depth32 && request_depth32) || !supports_depth24) {
      _depth_stencil_format = VK_FORMAT_D32_SFLOAT;
      _fb_properties.set_depth_bits(32);
    } else {
      _depth_stencil_format = VK_FORMAT_X8_D24_UNORM_PACK32;
      _fb_properties.set_depth_bits(24);
    }

    _depth_stencil_plane = RTP_depth;

  } else {
    _depth_stencil_format = VK_FORMAT_UNDEFINED;
    _depth_stencil_plane = RTP_depth_stencil;
  }

  if (_creation_flags & GraphicsPipe::BF_size_track_host) {
    GraphicsOutput *host = _host;
    nassertr(host != nullptr, false);
    _size = host->get_size();
  }

  return setup_render_pass() && create_framebuffer();
}

/**
 * Creates a render pass object for this buffer.  Call this whenever the
 * format or clear parameters change.  Note that all pipeline states become
 * invalid if the render pass is no longer compatible; however, we currently
 * call this only when the clear flags change, which does not affect pipeline
 * compatibility.
 */
bool VulkanGraphicsBuffer::
setup_render_pass() {
  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_R(vkgsg, _gsg, false);

  if (vulkandisplay_cat.is_debug()) {
    vulkandisplay_cat.debug()
      << "Creating render pass for VulkanGraphicsBuffer " << this << "\n";
  }

  nassertr(vkgsg->_frame_data == nullptr, false);

  // Check if we are planning on doing anything with the depth/color output.
  BitMask32 transfer_planes = 0;
  {
    CDReader cdata(_cycler);
    for (const auto &rt : cdata->_textures) {
      RenderTextureMode mode = rt._rtm_mode;
      if (mode == RTM_copy_texture || mode == RTM_copy_ram) {
        transfer_planes.set_bit(rt._plane);
      }
    }
  }
  transfer_planes = ~0;

  // Now we want to create a render pass, and for that we need to describe the
  // framebuffer attachments as well as any subpasses we'd like to use.
  VkAttachmentDescription attachments[2];
  uint32_t ai = 0;

  VkAttachmentReference color_reference;
  color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depth_reference;
  depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  if (_color_format != VK_FORMAT_UNDEFINED) {
    VkAttachmentDescription &attach = attachments[ai];
    attach.flags = 0;
    attach.format = _color_format;
    attach.samples = VK_SAMPLE_COUNT_1_BIT;
    attach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attach.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    if (get_clear_color_active()) {
      attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    } else {
      attach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    if (transfer_planes.get_bit(RTP_color)) {
      attach.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    color_reference.attachment = ai++;
  }

  if (_depth_stencil_format != VK_FORMAT_UNDEFINED) {
    VkAttachmentDescription &attach = attachments[ai];
    attach.flags = 0;
    attach.format = _depth_stencil_format;
    attach.samples = VK_SAMPLE_COUNT_1_BIT;
    attach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attach.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    if (get_clear_depth_active()) {
      attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    }
    if (get_clear_stencil_active()) {
      attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    }
    if (get_clear_depth_active() && get_clear_stencil_active()) {
      attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if (transfer_planes.get_bit(RTP_depth) || transfer_planes.get_bit(RTP_stencil) ||
        transfer_planes.get_bit(RTP_depth_stencil)) {
      attach.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    depth_reference.attachment = ai++;
  }

  VkSubpassDescription subpass;
  subpass.flags = 0;
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = nullptr;
  subpass.colorAttachmentCount = _color_format ? 1 : 0;
  subpass.pColorAttachments = &color_reference;
  subpass.pResolveAttachments = nullptr;
  subpass.pDepthStencilAttachment = _depth_stencil_format ? &depth_reference : nullptr;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = nullptr;

  VkRenderPassCreateInfo pass_info;
  pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  pass_info.pNext = nullptr;
  pass_info.flags = 0;
  pass_info.attachmentCount = ai;
  pass_info.pAttachments = attachments;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  pass_info.dependencyCount = 0;
  pass_info.pDependencies = nullptr;

  VkRenderPass pass;
  VkResult
  err = vkCreateRenderPass(vkgsg->_device, &pass_info, nullptr, &pass);
  if (err) {
    vulkan_error(err, "Failed to create render pass");
    return false;
  }

  // Destroy the previous render pass object.
  if (_render_pass != VK_NULL_HANDLE) {
    if (vkgsg->_last_frame_data != nullptr) {
      vkgsg->_last_frame_data->_pending_destroy_render_passes.push_back(_render_pass);
    } else {
      vkDestroyRenderPass(vkgsg->_device, _render_pass, nullptr);
    }
  }

  _render_pass = pass;
  _current_clear_mask = _clear_mask;
  return true;
}

/**
 * Destroys an existing swapchain.  Before calling this, make sure that no
 * commands are executing on any queue that uses this swapchain.
 */
void VulkanGraphicsBuffer::
destroy_framebuffer() {
  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_V(vkgsg, _gsg);
  VkDevice device = vkgsg->_device;

  // This shouldn't happen within a begin_frame/end_frame pair.
  nassertv(vkgsg->_frame_data == nullptr);

  // Make sure that the GSG's command buffer releases its resources.
  //if (vkgsg->_cmd != VK_NULL_HANDLE) {
  //  vkResetCommandBuffer(vkgsg->_cmd, 0);
  //}

  /*if (!_present_cmds.empty()) {
    vkFreeCommandBuffers(device, vkgsg->_cmd_pool, _present_cmds.size(), &_present_cmds[0]);
    _present_cmds.clear();
  }*/

  // Destroy the resources held for each attachment.
  for (Attachment &attach : _attachments) {
    if (vkgsg->_last_frame_data != nullptr) {
      attach._tc->release(*vkgsg->_last_frame_data);
    } else {
      attach._tc->destroy_now(device);
    }
    delete attach._tc;
  }
  _attachments.clear();

  if (_framebuffer != VK_NULL_HANDLE) {
    if (vkgsg->_last_frame_data != nullptr) {
      vkgsg->_last_frame_data->_pending_destroy_framebuffers.push_back(_framebuffer);
    } else {
      vkDestroyFramebuffer(device, _framebuffer, nullptr);
    }
    _framebuffer = VK_NULL_HANDLE;
  }

  _is_valid = false;
}

/**
 * Creates or recreates the framebuffer.
 */
bool VulkanGraphicsBuffer::
create_framebuffer() {
  VulkanGraphicsPipe *vkpipe;
  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_R(vkpipe, _pipe, false);
  DCAST_INTO_R(vkgsg, _gsg, false);
  VkDevice device = vkgsg->_device;
  VkResult err;

  if (!create_attachment(RTP_color, _color_format) ||
      !create_attachment(_depth_stencil_plane, _depth_stencil_format)) {
    return false;
  }

  uint32_t num_attachments = _attachments.size();
  VkImageView *attach_views = (VkImageView *)alloca(sizeof(VkImageView) * num_attachments);

  for (uint32_t i = 0; i < num_attachments; ++i) {
    attach_views[i] = _attachments[i]._tc->get_image_view(0);
  }

  VkFramebufferCreateInfo fb_info;
  fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_info.pNext = nullptr;
  fb_info.flags = 0;
  fb_info.renderPass = _render_pass;
  fb_info.attachmentCount = num_attachments;
  fb_info.pAttachments = attach_views;
  fb_info.width = _size[0];
  fb_info.height = _size[1];
  fb_info.layers = 1;

  err = vkCreateFramebuffer(device, &fb_info, nullptr, &_framebuffer);
  if (err) {
    vulkan_error(err, "Failed to create framebuffer");
    return false;
  }

  _framebuffer_size = _size;
  _is_valid = true;
  return true;
}

/**
 * Adds a new attachment to the framebuffer.
 * @return Returns true on success.
 */
bool VulkanGraphicsBuffer::
create_attachment(RenderTexturePlane plane, VkFormat format) {
  if (format == VK_FORMAT_UNDEFINED) {
    return true;
  }

  VulkanGraphicsPipe *vkpipe;
  DCAST_INTO_R(vkpipe, _pipe, false);

  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_R(vkgsg, _gsg, false);
  VkDevice device = vkgsg->_device;

  VkExtent3D extent;
  extent.width = _size[0];
  extent.height = _size[1];
  extent.depth = 1;

  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (plane == RTP_depth || plane == RTP_stencil || plane == RTP_depth_stencil) {
    usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  } else {
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }

  VulkanTextureContext *tc = new VulkanTextureContext(vkgsg->get_prepared_objects());
  if (!vkgsg->create_image(tc, VK_IMAGE_TYPE_2D, format, extent, 1, 1,
                           VK_SAMPLE_COUNT_1_BIT, usage)) {
    delete tc;
    return false;
  }

  VkImageViewCreateInfo view_info;
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.pNext = nullptr;
  view_info.flags = 0;
  view_info.image = tc->_image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = tc->_format;
  view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_info.subresourceRange.baseMipLevel = 0;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = 1;

  switch (plane) {
  default:
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    break;

  case RTP_depth:
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    break;

  case RTP_stencil:
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    break;

  case RTP_depth_stencil:
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    //                                      | VK_IMAGE_ASPECT_STENCIL_BIT;
    break;
  }
  tc->_aspect_mask = view_info.subresourceRange.aspectMask;

  VkImageView image_view;
  VkResult err;
  err = vkCreateImageView(device, &view_info, nullptr, &image_view);
  if (err) {
    vulkan_error(err, "Failed to create image view for attachment");
    delete tc;
    return false;
  }

  tc->_image_views.push_back(image_view);
  _attachments.push_back({tc, plane});
  return true;
}
