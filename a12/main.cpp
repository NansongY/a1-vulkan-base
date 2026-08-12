#include <volk/volk.h>

#include <print>
#include <chrono>
#include <limits>
#include <vector>
#include <utility>
#include <stdexcept>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if !defined(GLM_FORCE_RADIANS)
#	define GLM_FORCE_RADIANS
#endif
#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../labut2/angle.hpp"
using namespace labut2::literals;

#include "../labut2/load.hpp"
#include "../labut2/error.hpp"
#include "../labut2/synch.hpp"
#include "../labut2/vkimage.hpp"
#include "../labut2/commands.hpp"
#include "../labut2/textures.hpp"
#include "../labut2/vkbuffer.hpp"
#include "../labut2/vkobject.hpp"
#include "../labut2/to_string.hpp"
#include "../labut2/descriptors.hpp"
#include "../labut2/vulkan_window.hpp"
namespace lut = labut2;

#include "baked_model.hpp"


namespace
{
	constexpr std::uint64_t kFenceTimeout = 1000000000ULL * 120ULL;

	void throw_if_failed( VkResult aResult, char const* aCall )
	{
		if( VK_SUCCESS != aResult )
			throw lut::Error( "{} returned {}", aCall, lut::to_string(aResult) );
	}

	struct Camera
	{
		glm::vec3 position = glm::vec3( 0.f );
		float yaw = 0.f;    // radians, around Y
		float pitch = 0.f;  // radians, up/down
		float fov = glm::radians( 60.f );
		float nearPlane = 0.1f;
		float farPlane = 500.f;
		float moveSpeed = 5.f;   // world units / second
		float fastFactor = 4.f;  // Shift
		float slowFactor = 0.2f; // Ctrl
		float mouseSensitivity = 0.002f;

		glm::vec3 front() const
		{
			return glm::normalize( glm::vec3(
				std::cos( yaw ) * std::cos( pitch ),
				std::sin( pitch ),
				std::sin( yaw ) * std::cos( pitch )
			) );
		}

		glm::mat4 view() const
		{
			return glm::lookAt( position, position + front(), glm::vec3( 0.f, 1.f, 0.f ) );
		}
	};

	// Keyboard / mouse state, updated exclusively from GLFW callbacks
	// (the assignment requires event-driven input, not polling).
	struct InputState
	{
		bool forward = false, backward = false, left = false, right = false;
		bool up = false, down = false;
		bool fast = false, slow = false;
		bool mouseLook = false;
		bool firstMouse = true;
		double lastX = 0.0, lastY = 0.0;
	};

	// Everything stored in the GLFW window user pointer.
	struct ApplicationState
	{
		Camera camera;
		InputState input;
		bool framebufferResized = false;
		double lastFrameTime = 0.0;
		int renderMode = 1; // 1 = default, 2..4 = debug visualizations
	};

	void framebuffer_resized_callback( GLFWwindow* aWindow, int, int )
	{
		auto* app = static_cast<ApplicationState*>( glfwGetWindowUserPointer( aWindow ) );
		if( app )
			app->framebufferResized = true;
	}

	void key_callback( GLFWwindow* aWindow, int aKey, int, int aAction, int )
	{
		auto* app = static_cast<ApplicationState*>( glfwGetWindowUserPointer( aWindow ) );
		if( !app )
			return;

		// While a key is held, GLFW sends PRESS once, then REPEAT while held,
		// and RELEASE once when let go. Treat both PRESS and REPEAT as "held",
		// otherwise the flag gets cleared by the first REPEAT event and the
		// camera stops shortly after you start holding a key.
		bool const pressed = (GLFW_PRESS == aAction || GLFW_REPEAT == aAction);
		switch( aKey )
		{
			case GLFW_KEY_W: app->input.forward = pressed; break;
			case GLFW_KEY_S: app->input.backward = pressed; break;
			case GLFW_KEY_A: app->input.left = pressed; break;
			case GLFW_KEY_D: app->input.right = pressed; break;
			case GLFW_KEY_E: app->input.up = pressed; break;
			case GLFW_KEY_Q: app->input.down = pressed; break;
			case GLFW_KEY_LEFT_SHIFT:
			case GLFW_KEY_RIGHT_SHIFT: app->input.fast = pressed; break;
			case GLFW_KEY_LEFT_CONTROL:
			case GLFW_KEY_RIGHT_CONTROL: app->input.slow = pressed; break;
			// Task 1.4: main number keys switch the rendering mode (1..4).
			case GLFW_KEY_1: case GLFW_KEY_2: case GLFW_KEY_3: case GLFW_KEY_4:
				if( pressed )
					app->renderMode = aKey - GLFW_KEY_1 + 1;
				break;
			default: break;
		}
	}

	void mouse_button_callback( GLFWwindow* aWindow, int aButton, int aAction, int )
	{
		auto* app = static_cast<ApplicationState*>( glfwGetWindowUserPointer( aWindow ) );
		if( !app )
			return;

		// Right-click toggles mouse look (click again to disable).
		if( GLFW_MOUSE_BUTTON_RIGHT == aButton && GLFW_PRESS == aAction )
		{
			app->input.mouseLook = !app->input.mouseLook;
			glfwSetInputMode( aWindow, GLFW_CURSOR, app->input.mouseLook ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL );
			app->input.firstMouse = true;
		}
	}

	void cursor_pos_callback( GLFWwindow* aWindow, double aX, double aY )
	{
		auto* app = static_cast<ApplicationState*>( glfwGetWindowUserPointer( aWindow ) );
		if( !app || !app->input.mouseLook )
			return;

		if( app->input.firstMouse )
		{
			app->input.lastX = aX;
			app->input.lastY = aY;
			app->input.firstMouse = false;
		}

		double const dx = aX - app->input.lastX;
		double const dy = aY - app->input.lastY;
		app->input.lastX = aX;
		app->input.lastY = aY;

		Camera& cam = app->camera;
		cam.yaw   += float( dx ) * cam.mouseSensitivity; // mouse right -> turn right
		cam.pitch -= float( dy ) * cam.mouseSensitivity;

		float const limit = glm::radians( 89.f );
		cam.pitch = std::clamp( cam.pitch, -limit, limit );
	}

	// Moves the camera from the (event-driven) input state, frame-rate independent.
	void update_camera( ApplicationState& aApp )
	{
		Camera& cam = aApp.camera;
		InputState const& input = aApp.input;

		double const now = glfwGetTime();
		float const deltaTime = float( now - aApp.lastFrameTime );
		aApp.lastFrameTime = now;
		if( deltaTime <= 0.f )
			return;
		// Clamp dt so one slow frame can never cause a big jump; this keeps
		// movement continuous and frame-rate independent even at low FPS.
		float const dt = std::min( deltaTime, 0.05f );

		float speed = cam.moveSpeed;
		if( input.fast )
			speed *= cam.fastFactor;
		if( input.slow )
			speed *= cam.slowFactor;

		glm::vec3 const front = cam.front();
		glm::vec3 const horizontalFront = glm::normalize( glm::vec3( front.x, 0.f, front.z ) );
		glm::vec3 const right = glm::normalize( glm::cross( front, glm::vec3(0.f,1.f,0.f) ) );

		glm::vec3 move( 0.f );
		if( input.forward ) move += horizontalFront;
		if( input.backward ) move -= horizontalFront;
		if( input.right ) move += right;
		if( input.left ) move -= right;
		if( input.up ) move += glm::vec3(0.f,1.f,0.f);
		if( input.down ) move -= glm::vec3(0.f,1.f,0.f);

		if( glm::dot( move, move ) > 0.f )
			move = glm::normalize( move );

		cam.position += move * speed * dt;
	}

	void transition_image_layout( VkCommandBuffer aCmd, VkImage aImage, VkImageLayout aOldLayout, VkImageLayout aNewLayout, VkPipelineStageFlags2 aSrcStage, VkAccessFlags2 aSrcAccess, VkPipelineStageFlags2 aDstStage, VkAccessFlags2 aDstAccess, VkImageAspectFlags aAspect = VK_IMAGE_ASPECT_COLOR_BIT )
	{
		VkImageMemoryBarrier2 barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.srcStageMask = aSrcStage;
		barrier.srcAccessMask = aSrcAccess;
		barrier.dstStageMask = aDstStage;
		barrier.dstAccessMask = aDstAccess;
		barrier.oldLayout = aOldLayout;
		barrier.newLayout = aNewLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = aImage;
		barrier.subresourceRange.aspectMask = aAspect;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		VkDependencyInfo dependencyInfo{};
		dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dependencyInfo.imageMemoryBarrierCount = 1;
		dependencyInfo.pImageMemoryBarriers = &barrier;

		vkCmdPipelineBarrier2( aCmd, &dependencyInfo );
	}

	// Interleaved vertex format matching the baked model data.
	struct Vertex
	{
		glm::vec3 pos;
		glm::vec3 normal;
		glm::vec2 uv;
	};

	// Per-mesh GPU buffers (one per baked mesh).
	struct MeshResources
	{
		lut::Buffer vertexBuffer;
		lut::Buffer indexBuffer;
		std::uint32_t indexCount = 0;
		std::uint32_t materialId = 0;
	};

	// Per-frame scene data passed to the vertex AND fragment shaders (Task 1.5
	// PBR lighting data). Everything is a vec4 so the std140 layout is trivial.
	struct SceneUBO
	{
		glm::mat4 viewProj;     // offset 0
		glm::vec4 params;       // offset 64: x = near, y = far, z = renderMode
		glm::vec4 cameraPos;    // offset 80: camera position (world space)
		glm::vec4 lightPos;     // offset 96: light position (world space)
		glm::vec4 lightColor;   // offset 112: light color
		glm::vec4 ambientColor; // offset 128: ambient light color
	};

	// A linked vertex + fragment shader pair.
	struct ShaderPair
	{
		lut::Shader vertex;
		lut::Shader fragment;
	};

	// Resources that survive across frames.
	struct SceneResources
	{
		std::vector<MeshResources> meshes;
		std::vector<lut::Image> textures;
		std::vector<lut::ImageView> textureViews;
		lut::Sampler sampler;

		lut::Buffer uboBuffer;
		lut::DescriptorSetLayout uboSetLayout;       // set 0: per-frame UBO
		lut::DescriptorSetLayout materialSetLayout;  // set 1: per-material texture
		lut::DescriptorPool descriptorPool;
		lut::PipelineLayout pipelineLayout;
		VkDescriptorSet uboSet = VK_NULL_HANDLE;
		std::vector<VkDescriptorSet> materialSets;

		lut::Image depthImage;
		lut::ImageView depthView;

		ShaderPair mainShaders;   // default.vert + default.frag (mode 1)
		ShaderPair debugShaders;  // default.vert + debug.frag (modes 2..4)
	};

	// Creates the vertex and fragment shader objects (VK_EXT_shader_object).
	// We use VK_SHADER_CREATE_LINK_STAGE_BIT_EXT so both stages are linked
	// together (they must then be created in the same vkCreateShadersEXT call).
	// Note: this bundled header is VK_EXT_shader_object spec version 1, where
	// vertex-input and color-blend state are NOT part of VkShaderCreateInfoEXT
	// -- they are dynamic state that we set per command buffer instead.
	// aSet0/aSet1 are the two descriptor set layouts used by the shaders
	// (set 0 = UBO, set 1 = material texture). Both stages must be created
	// with identical set-layout arrays (VUID-vkCmdDrawIndexed-None-08879).
	ShaderPair create_shader_pair( VkDevice aDevice, std::vector<std::uint32_t> const& aVertCode, std::vector<std::uint32_t> const& aFragCode, VkDescriptorSetLayout aSet0, VkDescriptorSetLayout aSet1 )
	{
		VkDescriptorSetLayout const setLayouts[2] = { aSet0, aSet1 };

		VkShaderCreateInfoEXT infos[2]{};

		infos[0].sType  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
		infos[0].flags  = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
		infos[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		infos[0].nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
		infos[0].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
		infos[0].codeSize = aVertCode.size() * sizeof(std::uint32_t);
		infos[0].pCode = aVertCode.data();
		infos[0].pName = "main";
		infos[0].setLayoutCount = 2;
		infos[0].pSetLayouts = setLayouts;

		infos[1].sType  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
		infos[1].flags  = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
		infos[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		infos[1].nextStage = VkShaderStageFlagBits(0);
		infos[1].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
		infos[1].codeSize = aFragCode.size() * sizeof(std::uint32_t);
		infos[1].pCode = aFragCode.data();
		infos[1].pName = "main";
		infos[1].setLayoutCount = 2; // must match the vertex stage's set layouts
		infos[1].pSetLayouts = setLayouts;

		VkShaderEXT shaders[2]{};
		throw_if_failed( vkCreateShadersEXT( aDevice, 2, infos, nullptr, shaders ), "vkCreateShadersEXT()" );
		return { lut::Shader( aDevice, shaders[0] ), lut::Shader( aDevice, shaders[1] ) };
	}

	// Trilinear sampler (min/mag = LINEAR, mipmap = LINEAR) for the textures.
	lut::Sampler create_texture_sampler( VkDevice aDevice )
	{
		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_LINEAR;
		info.minFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		info.mipLodBias = 0.f;
		info.anisotropyEnable = VK_FALSE; // disabled (assignment wants this off for the mip visualization task)
		info.maxAnisotropy = 1.f;
		info.compareEnable = VK_FALSE;
		info.minLod = 0.f;
		info.maxLod = VK_LOD_CLAMP_NONE; // clamped by each texture's own mip count
		info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		info.unnormalizedCoordinates = VK_FALSE;

		VkSampler sampler = VK_NULL_HANDLE;
		throw_if_failed( vkCreateSampler( aDevice, &info, nullptr, &sampler ), "vkCreateSampler()" );
		return lut::Sampler( aDevice, sampler );
	}

	// Full mip-chain 2D image view for a texture.
	lut::ImageView create_texture_view( VkDevice aDevice, VkImage aImage, VkFormat aFormat )
	{
		VkImageViewCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		info.image = aImage;
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = aFormat;
		info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.subresourceRange.baseMipLevel = 0;
		info.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS; // cover the whole mip chain
		info.subresourceRange.baseArrayLayer = 0;
		info.subresourceRange.layerCount = 1;

		VkImageView view = VK_NULL_HANDLE;
		throw_if_failed( vkCreateImageView( aDevice, &info, nullptr, &view ), "vkCreateImageView()" );
		return lut::ImageView( aDevice, view );
	}

	// Submits a small one-shot command buffer (used for setup-time transfers)
	// and blocks until it has finished.
	template< typename tRecordFn >
	void submit_one_time( lut::VulkanWindow const& aWindow, VkCommandPool aCmdPool, tRecordFn&& aRecord )
	{
		auto const cmd = lut::alloc_command_buffer( aWindow, aCmdPool );

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		throw_if_failed( vkBeginCommandBuffer( cmd, &beginInfo ), "vkBeginCommandBuffer()" );

		aRecord( cmd );

		throw_if_failed( vkEndCommandBuffer( cmd ), "vkEndCommandBuffer()" );

		VkCommandBufferSubmitInfo cmdInfo{};
		cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
		cmdInfo.commandBuffer = cmd;

		VkSubmitInfo2 submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
		submitInfo.commandBufferInfoCount = 1;
		submitInfo.pCommandBufferInfos = &cmdInfo;

		auto fence = lut::create_fence( aWindow.device );
		throw_if_failed( vkQueueSubmit2( aWindow.graphicsQueue, 1, &submitInfo, fence.handle ), "vkQueueSubmit2()" );
		throw_if_failed( vkWaitForFences( aWindow.device, 1, &fence.handle, VK_TRUE, kFenceTimeout ), "vkWaitForFences()" );
	}

	// Copies data into a staging buffer and uploads it to a device-local buffer.
	lut::Buffer upload_buffer( lut::VulkanWindow const& aWindow, lut::Allocator const& aAllocator, VkCommandPool aCmdPool, void const* aData, VkDeviceSize aSize, VkBufferUsageFlags aUsage )
	{
		// 1) Host-visible staging buffer filled with the data.
		auto staging = lut::create_buffer(
			aAllocator,
			aSize,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
			VMA_MEMORY_USAGE_AUTO
		);
		{
			void* data = nullptr;
			throw_if_failed( vmaMapMemory( aAllocator.allocator, staging.allocation, &data ), "vmaMapMemory()" );
			std::memcpy( data, aData, aSize );
			vmaUnmapMemory( aAllocator.allocator, staging.allocation );
		}

		// 2) Device-local buffer that the GPU actually reads from.
		auto deviceBuffer = lut::create_buffer(
			aAllocator,
			aSize,
			aUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			0,
			VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
		);

		// 3) Record a copy and wait for it to finish.
		submit_one_time( aWindow, aCmdPool, [&]( VkCommandBuffer aCmd )
		{
			VkBufferCopy region{};
			region.size = aSize;
			vkCmdCopyBuffer( aCmd, staging.buffer, deviceBuffer.buffer, 1, &region );
		} );

		return deviceBuffer;
	}

	void record_draw_commands( lut::VulkanWindow const& aWindow, VkCommandBuffer aCmd, std::uint32_t aImageIndex, VkImageLayout aOldLayout, SceneResources const& aResources, int aRenderMode )
	{
		throw_if_failed( vkResetCommandBuffer( aCmd, 0 ), "vkResetCommandBuffer()" );

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		throw_if_failed( vkBeginCommandBuffer( aCmd, &beginInfo ), "vkBeginCommandBuffer()" );

		transition_image_layout(
			aCmd,
			aWindow.swapImages[aImageIndex],
			aOldLayout,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE,
			VK_ACCESS_2_NONE,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
		);

		VkClearValue clearValue{};
		clearValue.color.float32[0] = 0.025f;
		clearValue.color.float32[1] = 0.035f;
		clearValue.color.float32[2] = 0.055f;
		clearValue.color.float32[3] = 1.f;

		VkRenderingAttachmentInfo colorAttachment{};
		colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAttachment.imageView = aWindow.swapViews[aImageIndex];
		colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.clearValue = clearValue;

		// Depth attachment (cleared to "far" = 1.0 each frame).
		VkRenderingAttachmentInfo depthAttachment{};
		depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAttachment.imageView = aResources.depthView.handle;
		depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.clearValue.depthStencil.depth = 1.0f;

		VkRenderingInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea.offset = { 0, 0 };
		renderingInfo.renderArea.extent = aWindow.swapchainExtent;
		renderingInfo.layerCount = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments = &colorAttachment;
		renderingInfo.pDepthAttachment = &depthAttachment;

		vkCmdBeginRendering( aCmd, &renderingInfo );

		// With shader objects, viewport/scissor/topology are dynamic state:
		// we have to set them explicitly before drawing.
		VkViewport viewport{};
		viewport.width = static_cast<float>( aWindow.swapchainExtent.width );
		viewport.height = static_cast<float>( aWindow.swapchainExtent.height );
		viewport.maxDepth = 1.f;
		vkCmdSetViewportWithCount( aCmd, 1, &viewport );

		VkRect2D scissor{};
		scissor.extent = aWindow.swapchainExtent;
		vkCmdSetScissorWithCount( aCmd, 1, &scissor );

		// With shader objects, all these rasterization states are dynamic and
		// must be set explicitly before drawing (v1 VK_EXT_shader_object).
		vkCmdSetRasterizerDiscardEnable( aCmd, VK_FALSE );
		vkCmdSetCullMode( aCmd, VK_CULL_MODE_NONE );
		vkCmdSetFrontFace( aCmd, VK_FRONT_FACE_COUNTER_CLOCKWISE );
		vkCmdSetPrimitiveTopology( aCmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST );
		// Enable depth testing/writing against the bound depth attachment.
		// With shader objects the compare op is ALSO dynamic state -- set it to
		// LESS explicitly (depth buffer is cleared to 1.0 == "far" each frame),
		// otherwise the default compare op can reject every fragment.
		vkCmdSetDepthCompareOp( aCmd, VK_COMPARE_OP_LESS );
		vkCmdSetDepthTestEnable( aCmd, VK_TRUE );
		vkCmdSetDepthWriteEnable( aCmd, VK_TRUE );
		vkCmdSetDepthBoundsTestEnable( aCmd, VK_FALSE );
		vkCmdSetStencilTestEnable( aCmd, VK_FALSE );
		vkCmdSetDepthBiasEnable( aCmd, VK_FALSE );
		vkCmdSetPrimitiveRestartEnable( aCmd, VK_FALSE );
		vkCmdSetPolygonModeEXT( aCmd, VK_POLYGON_MODE_FILL );
		vkCmdSetRasterizationSamplesEXT( aCmd, VK_SAMPLE_COUNT_1_BIT );
		VkSampleMask const sampleMask = 0x1;
		vkCmdSetSampleMaskEXT( aCmd, VK_SAMPLE_COUNT_1_BIT, &sampleMask );
		vkCmdSetAlphaToCoverageEnableEXT( aCmd, VK_FALSE );

		// Vertex input is dynamic state with (v1) shader objects.
		VkVertexInputBindingDescription2EXT binding{};
		binding.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
		binding.binding = 0;
		binding.stride = sizeof(Vertex);
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		binding.divisor = 1; // required (divisor 0 needs a separate feature)

		VkVertexInputAttributeDescription2EXT attribs[3]{};
		attribs[0].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
		attribs[0].location = 0;
		attribs[0].binding = 0;
		attribs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		attribs[0].offset = offsetof(Vertex, pos);

		attribs[1].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
		attribs[1].location = 1;
		attribs[1].binding = 0;
		attribs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		attribs[1].offset = offsetof(Vertex, normal);

		attribs[2].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
		attribs[2].location = 2;
		attribs[2].binding = 0;
		attribs[2].format = VK_FORMAT_R32G32_SFLOAT; // vec2
		attribs[2].offset = offsetof(Vertex, uv);

		vkCmdSetVertexInputEXT( aCmd, 1, &binding, 3, attribs );

		// Color blend is dynamic state with (v1) shader objects.
		VkBool32 const colorBlendEnable = VK_FALSE;
		vkCmdSetColorBlendEnableEXT( aCmd, 0, 1, &colorBlendEnable );

		VkColorBlendEquationEXT blendEquation{};
		blendEquation.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blendEquation.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendEquation.colorBlendOp = VK_BLEND_OP_ADD;
		blendEquation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendEquation.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendEquation.alphaBlendOp = VK_BLEND_OP_ADD;
		vkCmdSetColorBlendEquationEXT( aCmd, 0, 1, &blendEquation );

		VkColorComponentFlags const colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		vkCmdSetColorWriteMaskEXT( aCmd, 0, 1, &colorWriteMask );

		// Bind either the main or the debug (Task 1.4) shader pair.
		ShaderPair const& pair = (aRenderMode > 1) ? aResources.debugShaders : aResources.mainShaders;
		VkShaderStageFlagBits stages[] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };
		VkShaderEXT shaders[] = { pair.vertex.handle, pair.fragment.handle };
		vkCmdBindShadersEXT( aCmd, 2, stages, shaders );

		// Bind the per-frame UBO (descriptor set 0) through the pipeline layout.
		vkCmdBindDescriptorSets( aCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aResources.pipelineLayout.handle, 0, 1, &aResources.uboSet, 0, nullptr );

		// Draw every mesh with its own vertex + index buffers (indexed drawing),
		// binding the material's texture descriptor set (set 1) first.
		VkDeviceSize offset = 0;
		for( auto const& mesh : aResources.meshes )
		{
			VkDescriptorSet const materialSet = aResources.materialSets[mesh.materialId];
			vkCmdBindDescriptorSets( aCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aResources.pipelineLayout.handle, 1, 1, &materialSet, 0, nullptr );

			vkCmdBindVertexBuffers( aCmd, 0, 1, &mesh.vertexBuffer.buffer, &offset );
			vkCmdBindIndexBuffer( aCmd, mesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32 );
			vkCmdDrawIndexed( aCmd, mesh.indexCount, 1, 0, 0, 0 );
		}

		vkCmdEndRendering( aCmd );

		transition_image_layout(
			aCmd,
			aWindow.swapImages[aImageIndex],
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_NONE,
			VK_ACCESS_2_NONE
		);

		throw_if_failed( vkEndCommandBuffer( aCmd ), "vkEndCommandBuffer()" );
	}

	// Creates (or recreates) the depth image + view for the current swapchain
	// extent. The depth attachment must be recreated whenever the window resizes.
	void create_depth_resources( lut::VulkanWindow const& aWindow, lut::Allocator const& aAllocator, VkCommandPool aCmdPool, lut::Image& aImage, lut::ImageView& aView )
	{
		VkFormat const format = VK_FORMAT_D32_SFLOAT;

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = format;
		imageInfo.extent.width = aWindow.swapchainExtent.width;
		imageInfo.extent.height = aWindow.swapchainExtent.height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		throw_if_failed( vmaCreateImage( aAllocator.allocator, &imageInfo, &allocInfo, &image, &allocation, nullptr ), "vmaCreateImage()" );
		aImage = lut::Image( aAllocator.allocator, image, allocation );

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;

		VkImageView view = VK_NULL_HANDLE;
		throw_if_failed( vkCreateImageView( aWindow.device, &viewInfo, nullptr, &view ), "vkCreateImageView()" );
		aView = lut::ImageView( aWindow.device, view );

		// UNDEFINED -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL (one-time).
		submit_one_time( aWindow, aCmdPool, [&]( VkCommandBuffer aCmd )
		{
			transition_image_layout(
				aCmd,
				image,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_PIPELINE_STAGE_2_NONE,
				VK_ACCESS_2_NONE,
				VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
				VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				VK_IMAGE_ASPECT_DEPTH_BIT
			);
		} );
	}

	void reset_swapchain_frame_resources( lut::VulkanWindow const& aWindow, std::vector<VkImageLayout>& aLayouts, std::vector<lut::Semaphore>& aRenderFinished )
	{
		aLayouts.assign( aWindow.swapImages.size(), VK_IMAGE_LAYOUT_UNDEFINED );
		aRenderFinished.clear();
		aRenderFinished.reserve( aWindow.swapImages.size() );
		for( std::size_t i = 0; i < aWindow.swapImages.size(); ++i )
			aRenderFinished.emplace_back( lut::create_semaphore( aWindow.device ) );
	}

	void draw_frame( lut::VulkanWindow& aWindow, VkCommandBuffer aCmd, SceneResources& aResources, lut::Allocator const& aAllocator, VkCommandPool aCmdPool, lut::Semaphore const& aImageAvailable, std::vector<lut::Semaphore>& aRenderFinished, lut::Fence const& aInFlight, std::vector<VkImageLayout>& aSwapImageLayouts, ApplicationState& aAppState )
	{
		throw_if_failed( vkWaitForFences( aWindow.device, 1, &aInFlight.handle, VK_TRUE, kFenceTimeout ), "vkWaitForFences()" );

		std::uint32_t imageIndex = 0;
		auto const acquireRes = vkAcquireNextImageKHR( aWindow.device, aWindow.swapchain, std::numeric_limits<std::uint64_t>::max(), aImageAvailable.handle, VK_NULL_HANDLE, &imageIndex );
		if( VK_ERROR_OUT_OF_DATE_KHR == acquireRes )
		{
			lut::recreate_swapchain( aWindow );
			create_depth_resources( aWindow, aAllocator, aCmdPool, aResources.depthImage, aResources.depthView );
			reset_swapchain_frame_resources( aWindow, aSwapImageLayouts, aRenderFinished );
			return;
		}

		if( VK_SUCCESS != acquireRes && VK_SUBOPTIMAL_KHR != acquireRes )
			throw lut::Error( "vkAcquireNextImageKHR() returned {}", lut::to_string(acquireRes) );

		throw_if_failed( vkResetFences( aWindow.device, 1, &aInFlight.handle ), "vkResetFences()" );

		// Per-frame UBO: view-projection from the current camera.
		{
			Camera const& cam = aAppState.camera;
			float const aspect = float( aWindow.swapchainExtent.width ) / float( aWindow.swapchainExtent.height );
			glm::mat4 proj = glm::perspective( cam.fov, aspect, cam.nearPlane, cam.farPlane );
			proj[1][1] *= -1.f; // Vulkan NDC has a flipped Y axis

			SceneUBO ubo{};
			ubo.viewProj = proj * cam.view();
			ubo.params = glm::vec4( cam.nearPlane, cam.farPlane, float( aAppState.renderMode ), 0.f );
			// Task 1.5: PBR lighting data. Passed via the UBO (not hard-coded in
			// the shaders); the light position/color follow the assignment.
			ubo.cameraPos = glm::vec4( cam.position, 1.f );
			ubo.lightPos = glm::vec4( -0.2972f, 7.3100f, -11.9532f, 1.f );
			ubo.lightColor = glm::vec4( 1.f, 1.f, 1.f, 1.f );
			ubo.ambientColor = glm::vec4( 0.02f, 0.02f, 0.02f, 1.f );

			void* data = nullptr;
			throw_if_failed( vmaMapMemory( aAllocator.allocator, aResources.uboBuffer.allocation, &data ), "vmaMapMemory()" );
			std::memcpy( data, &ubo, sizeof(ubo) );
			vmaUnmapMemory( aAllocator.allocator, aResources.uboBuffer.allocation );
			vmaFlushAllocation( aAllocator.allocator, aResources.uboBuffer.allocation, 0, VK_WHOLE_SIZE );
		}

		record_draw_commands( aWindow, aCmd, imageIndex, aSwapImageLayouts[imageIndex], aResources, aAppState.renderMode );
		aSwapImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkSemaphoreSubmitInfo waitInfo{};
		waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		waitInfo.semaphore = aImageAvailable.handle;
		waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

		VkCommandBufferSubmitInfo cmdInfo{};
		cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
		cmdInfo.commandBuffer = aCmd;

		VkSemaphoreSubmitInfo signalInfo{};
		signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		signalInfo.semaphore = aRenderFinished[imageIndex].handle;
		signalInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

		VkSubmitInfo2 submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
		submitInfo.waitSemaphoreInfoCount = 1;
		submitInfo.pWaitSemaphoreInfos = &waitInfo;
		submitInfo.commandBufferInfoCount = 1;
		submitInfo.pCommandBufferInfos = &cmdInfo;
		submitInfo.signalSemaphoreInfoCount = 1;
		submitInfo.pSignalSemaphoreInfos = &signalInfo;

		throw_if_failed( vkQueueSubmit2( aWindow.graphicsQueue, 1, &submitInfo, aInFlight.handle ), "vkQueueSubmit2()" );

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &aRenderFinished[imageIndex].handle;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &aWindow.swapchain;
		presentInfo.pImageIndices = &imageIndex;

		auto const presentRes = vkQueuePresentKHR( aWindow.presentQueue, &presentInfo );
		if( VK_ERROR_OUT_OF_DATE_KHR == presentRes || VK_SUBOPTIMAL_KHR == presentRes || VK_SUBOPTIMAL_KHR == acquireRes || aAppState.framebufferResized )
		{
			aAppState.framebufferResized = false;
			lut::recreate_swapchain( aWindow );
			create_depth_resources( aWindow, aAllocator, aCmdPool, aResources.depthImage, aResources.depthView );
			reset_swapchain_frame_resources( aWindow, aSwapImageLayouts, aRenderFinished );
		}
		else if( VK_SUCCESS != presentRes )
		{
			throw lut::Error( "vkQueuePresentKHR() returned {}", lut::to_string(presentRes) );
		}
	}

	// Local types/structures:

}

int main() try
{
	auto window = lut::make_vulkan_window();

	// Application state (camera + input) lives in the window user pointer so
	// the GLFW callbacks can reach it.
	ApplicationState app;
	app.lastFrameTime = glfwGetTime();
	glfwSetWindowUserPointer( window.window, &app );
	glfwSetFramebufferSizeCallback( window.window, framebuffer_resized_callback );
	glfwSetKeyCallback( window.window, key_callback );
	glfwSetMouseButtonCallback( window.window, mouse_button_callback );
	glfwSetCursorPosCallback( window.window, cursor_pos_callback );

	auto allocator = lut::create_allocator( window );

	auto commandPool = lut::create_command_pool( window, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT );
	auto commandBuffer = lut::alloc_command_buffer( window, commandPool.handle );

	auto imageAvailable = lut::create_semaphore( window.device );
	auto inFlight = lut::create_fence( window.device, VK_FENCE_CREATE_SIGNALED_BIT );

	// --- Stage 1: load the baked model + textures, upload to GPU ---

	// 1) Load the model produced by the a12-bake tool.
	auto model = load_baked_model( "assets/a12/suntemple202526.comp5892mesh" );
	std::print( "Loaded model: {} meshes, {} materials, {} textures\n", model.meshes.size(), model.materials.size(), model.textures.size() );

	SceneResources resources;

	// 2) Upload each mesh: interleave the baked arrays into one Vertex array,
	//    then upload vertex + index buffers through a staging buffer.
	for( auto const& mesh : model.meshes )
	{
		std::vector<Vertex> vertices;
		vertices.reserve( mesh.positions.size() );
		for( std::size_t i = 0; i < mesh.positions.size(); ++i )
			vertices.push_back( Vertex{ mesh.positions[i], mesh.normals[i], mesh.texcoords[i] } );

		MeshResources mr;
		mr.vertexBuffer = upload_buffer( window, allocator, commandPool.handle, vertices.data(), vertices.size()*sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT );
		mr.indexBuffer = upload_buffer( window, allocator, commandPool.handle, mesh.indices.data(), mesh.indices.size()*sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT );
		mr.indexCount = std::uint32_t( mesh.indices.size() );
		mr.materialId = mesh.materialId;
		resources.meshes.emplace_back( std::move(mr) );
	}
	std::print( "Uploaded {} meshes ({} total vertices)\n", resources.meshes.size(), model.meshes.size() );

	// 3) Load every texture into GPU memory (with a full mip chain) and create
	//    a matching image view for each. sRGB textures get an sRGB format so
	//    the hardware decodes them to linear when sampling.
	resources.textures.reserve( model.textures.size() );
	resources.textureViews.reserve( model.textures.size() );
	for( auto const& tex : model.textures )
	{
		VkFormat const format = (ETextureSpace::srgb == tex.space)
			? VK_FORMAT_R8G8B8A8_SRGB
			: VK_FORMAT_R8G8B8A8_UNORM;

		resources.textures.emplace_back( lut::load_image_texture2d( tex.path.c_str(), window, commandPool.handle, allocator, format ) );
		resources.textureViews.emplace_back( create_texture_view( window.device, resources.textures.back().image, format ) );
	}
	resources.sampler = create_texture_sampler( window.device );
	std::print( "Loaded {} textures\n", resources.textures.size() );

	// 4) Initial camera: aim at the scene using the model's bounding box.
	glm::vec3 aabbMin( std::numeric_limits<float>::max() );
	glm::vec3 aabbMax( std::numeric_limits<float>::lowest() );
	for( auto const& mesh : model.meshes )
		for( auto const& p : mesh.positions )
		{
			aabbMin = glm::min( aabbMin, p );
			aabbMax = glm::max( aabbMax, p );
		}

	glm::vec3 const center = (aabbMin + aabbMax) * 0.5f;
	float const radius = glm::length( aabbMax - aabbMin ) * 0.5f;

	Camera& camera = app.camera;
	camera.position = center + glm::vec3( 0.f, radius*0.5f, radius / std::tan( camera.fov*0.5f ) * 1.6f );
	camera.farPlane = glm::length( center - camera.position ) + radius*4.f;
	camera.moveSpeed = std::max( 1.f, radius * 0.1f ); // cross the scene in ~5s at shift speed

	// Aim the initial view direction at the scene center (derive yaw/pitch).
	glm::vec3 const dir = glm::normalize( center - camera.position );
	camera.yaw = std::atan2( dir.z, dir.x );
	camera.pitch = std::asin( dir.y );

	// 5) Per-frame UBO buffer (contents are written every frame in draw_frame).
	resources.uboBuffer = lut::create_buffer(
		allocator,
		sizeof(SceneUBO),
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		VMA_MEMORY_USAGE_AUTO
	);

	// 6) Descriptor sets + pipeline layout.
	//    set 0: per-frame UBO (uniform buffer, vertex stage)
	//    set 1: per-material base color texture (combined image sampler, fragment stage)
	VkDescriptorSetLayoutBinding uboBinding{};
	uboBinding.binding = 0;
	uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	uboBinding.descriptorCount = 1;
	// Used by the vertex shader (viewProj) AND the debug fragment shader
	// (near/far/renderMode), so expose it to both stages.
	uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo uboLayoutInfo{};
	uboLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	uboLayoutInfo.bindingCount = 1;
	uboLayoutInfo.pBindings = &uboBinding;

	VkDescriptorSetLayout uboLayout = VK_NULL_HANDLE;
	throw_if_failed( vkCreateDescriptorSetLayout( window.device, &uboLayoutInfo, nullptr, &uboLayout ), "vkCreateDescriptorSetLayout()" );
	resources.uboSetLayout = lut::DescriptorSetLayout( window.device, uboLayout );

	// Task 1.5: the material set now exposes three textures -- base color
	// (binding 0), roughness (binding 1) and metalness (binding 2).
	VkDescriptorSetLayoutBinding texBindings[3]{};
	texBindings[0].binding = 0;
	texBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	texBindings[0].descriptorCount = 1;
	texBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	texBindings[1].binding = 1;
	texBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	texBindings[1].descriptorCount = 1;
	texBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	texBindings[2].binding = 2;
	texBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	texBindings[2].descriptorCount = 1;
	texBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo matLayoutInfo{};
	matLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	matLayoutInfo.bindingCount = 3;
	matLayoutInfo.pBindings = texBindings;

	VkDescriptorSetLayout matLayout = VK_NULL_HANDLE;
	throw_if_failed( vkCreateDescriptorSetLayout( window.device, &matLayoutInfo, nullptr, &matLayout ), "vkCreateDescriptorSetLayout()" );
	resources.materialSetLayout = lut::DescriptorSetLayout( window.device, matLayout );

	// Pool: 1 UBO set + one texture set per material.
	std::uint32_t const materialCount = std::uint32_t( model.materials.size() );

	VkDescriptorPoolSize poolSizes[2]{};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[0].descriptorCount = 1;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[1].descriptorCount = 3 * materialCount; // baseColor + roughness + metalness

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1 + materialCount;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;

	VkDescriptorPool pool = VK_NULL_HANDLE;
	throw_if_failed( vkCreateDescriptorPool( window.device, &poolInfo, nullptr, &pool ), "vkCreateDescriptorPool()" );
	resources.descriptorPool = lut::DescriptorPool( window.device, pool );

	// Allocate + write the single UBO set (set 0).
	VkDescriptorSetAllocateInfo setAllocInfo{};
	setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setAllocInfo.descriptorPool = pool;
	setAllocInfo.descriptorSetCount = 1;
	setAllocInfo.pSetLayouts = &uboLayout;
	throw_if_failed( vkAllocateDescriptorSets( window.device, &setAllocInfo, &resources.uboSet ), "vkAllocateDescriptorSets()" );

	VkDescriptorBufferInfo uboInfo{};
	uboInfo.buffer = resources.uboBuffer.buffer;
	uboInfo.range = sizeof(SceneUBO);

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = resources.uboSet;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	write.pBufferInfo = &uboInfo;
	vkUpdateDescriptorSets( window.device, 1, &write, 0, nullptr );

	// Allocate + write one texture set per material (set 1): base color,
	// roughness and metalness. If a material lacks a roughness/metalness map
	// (index 0xffffffff), fall back to its base color texture so every
	// descriptor references a valid image view.
	resources.materialSets.resize( materialCount );
	for( std::uint32_t m = 0; m < materialCount; ++m )
	{
		VkDescriptorSetAllocateInfo matAllocInfo{};
		matAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		matAllocInfo.descriptorPool = pool;
		matAllocInfo.descriptorSetCount = 1;
		matAllocInfo.pSetLayouts = &matLayout;
		throw_if_failed( vkAllocateDescriptorSets( window.device, &matAllocInfo, &resources.materialSets[m] ), "vkAllocateDescriptorSets()" );

		auto const fallbackId = [&model]( std::uint32_t aId, std::uint32_t aBaseId ) -> std::uint32_t
		{
			return (0xffffffffu == aId) ? aBaseId : aId;
		};
		std::uint32_t const baseColorId = model.materials[m].baseColorTextureId;
		std::uint32_t const roughId = fallbackId( model.materials[m].roughnessTextureId, baseColorId );
		std::uint32_t const metalId = fallbackId( model.materials[m].metalnessTextureId, baseColorId );

		VkDescriptorImageInfo imgInfos[3]{};
		for( auto& ii : imgInfos )
		{
			ii.sampler = resources.sampler.handle;
			ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		imgInfos[0].imageView = resources.textureViews[baseColorId].handle;
		imgInfos[1].imageView = resources.textureViews[roughId].handle;
		imgInfos[2].imageView = resources.textureViews[metalId].handle;

		VkWriteDescriptorSet matWrites[3]{};
		for( std::uint32_t b = 0; b < 3; ++b )
		{
			matWrites[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			matWrites[b].dstSet = resources.materialSets[m];
			matWrites[b].dstBinding = b;
			matWrites[b].descriptorCount = 1;
			matWrites[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			matWrites[b].pImageInfo = &imgInfos[b];
		}
		vkUpdateDescriptorSets( window.device, 3, matWrites, 0, nullptr );
	}

	// Pipeline layout covering both sets.
	VkDescriptorSetLayout const pipelineSetLayouts[2] = { uboLayout, matLayout };

	VkPipelineLayoutCreateInfo plInfo{};
	plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plInfo.setLayoutCount = 2;
	plInfo.pSetLayouts = pipelineSetLayouts;

	VkPipelineLayout pl = VK_NULL_HANDLE;
	throw_if_failed( vkCreatePipelineLayout( window.device, &plInfo, nullptr, &pl ), "vkCreatePipelineLayout()" );
	resources.pipelineLayout = lut::PipelineLayout( window.device, pl );

	// 7) Load shaders and create the shader pairs (set 0 = UBO, set 1 = texture).
	auto const vertCode = lut::load_file_u32( "assets/a12/shaders/default.vert.spv" );
	auto const fragCode = lut::load_file_u32( "assets/a12/shaders/default.frag.spv" );
	resources.mainShaders = create_shader_pair( window.device, vertCode, fragCode, uboLayout, matLayout );

	// Task 1.4: a second, independent shader pair for debug visualization
	// (same vertex shader, separate debug.frag -- not part of the main shader).
	auto const debugFragCode = lut::load_file_u32( "assets/a12/shaders/debug.frag.spv" );
	resources.debugShaders = create_shader_pair( window.device, vertCode, debugFragCode, uboLayout, matLayout );

	// 8) Depth buffer sized to the swapchain.
	create_depth_resources( window, allocator, commandPool.handle, resources.depthImage, resources.depthView );

	std::vector<VkImageLayout> swapImageLayouts;
	std::vector<lut::Semaphore> renderFinished;
	reset_swapchain_frame_resources( window, swapImageLayouts, renderFinished );

	while( !glfwWindowShouldClose( window.window ) )
	{
		glfwPollEvents();
		update_camera( app );
		draw_frame( window, commandBuffer, resources, allocator, commandPool.handle, imageAvailable, renderFinished, inFlight, swapImageLayouts, app );
	}

	throw_if_failed( vkDeviceWaitIdle( window.device ), "vkDeviceWaitIdle()" );


	return 0;
}
catch( std::exception const& eErr )
{
	std::print( stderr, "\n" );
	std::print( stderr, "Error: {}\n", eErr.what() );
	return 1;
}


//EOF vim:syntax=cpp:foldmethod=marker:ts=4:noexpandtab: 
