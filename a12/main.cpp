#include <volk/volk.h>

#include <print>
#include <chrono>
#include <limits>
#include <vector>
#include <stdexcept>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

	void framebuffer_resized_callback( GLFWwindow* aWindow, int, int )
	{
		auto* resized = static_cast<bool*>( glfwGetWindowUserPointer( aWindow ) );
		if( resized )
			*resized = true;
	}

	void transition_image_layout( VkCommandBuffer aCmd, VkImage aImage, VkImageLayout aOldLayout, VkImageLayout aNewLayout, VkPipelineStageFlags2 aSrcStage, VkAccessFlags2 aSrcAccess, VkPipelineStageFlags2 aDstStage, VkAccessFlags2 aDstAccess )
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
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
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

	void record_clear_commands( lut::VulkanWindow const& aWindow, VkCommandBuffer aCmd, std::uint32_t aImageIndex, VkImageLayout aOldLayout )
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

		VkRenderingInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea.offset = { 0, 0 };
		renderingInfo.renderArea.extent = aWindow.swapchainExtent;
		renderingInfo.layerCount = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments = &colorAttachment;

		vkCmdBeginRendering( aCmd, &renderingInfo );
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

	void reset_swapchain_frame_resources( lut::VulkanWindow const& aWindow, std::vector<VkImageLayout>& aLayouts, std::vector<lut::Semaphore>& aRenderFinished )
	{
		aLayouts.assign( aWindow.swapImages.size(), VK_IMAGE_LAYOUT_UNDEFINED );
		aRenderFinished.clear();
		aRenderFinished.reserve( aWindow.swapImages.size() );
		for( std::size_t i = 0; i < aWindow.swapImages.size(); ++i )
			aRenderFinished.emplace_back( lut::create_semaphore( aWindow.device ) );
	}

	void draw_frame( lut::VulkanWindow& aWindow, VkCommandBuffer aCmd, lut::Semaphore const& aImageAvailable, std::vector<lut::Semaphore>& aRenderFinished, lut::Fence const& aInFlight, std::vector<VkImageLayout>& aSwapImageLayouts, bool& aFramebufferResized )
	{
		throw_if_failed( vkWaitForFences( aWindow.device, 1, &aInFlight.handle, VK_TRUE, kFenceTimeout ), "vkWaitForFences()" );

		std::uint32_t imageIndex = 0;
		auto const acquireRes = vkAcquireNextImageKHR( aWindow.device, aWindow.swapchain, std::numeric_limits<std::uint64_t>::max(), aImageAvailable.handle, VK_NULL_HANDLE, &imageIndex );
		if( VK_ERROR_OUT_OF_DATE_KHR == acquireRes )
		{
			lut::recreate_swapchain( aWindow );
			reset_swapchain_frame_resources( aWindow, aSwapImageLayouts, aRenderFinished );
			return;
		}

		if( VK_SUCCESS != acquireRes && VK_SUBOPTIMAL_KHR != acquireRes )
			throw lut::Error( "vkAcquireNextImageKHR() returned {}", lut::to_string(acquireRes) );

		throw_if_failed( vkResetFences( aWindow.device, 1, &aInFlight.handle ), "vkResetFences()" );
		record_clear_commands( aWindow, aCmd, imageIndex, aSwapImageLayouts[imageIndex] );
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
		if( VK_ERROR_OUT_OF_DATE_KHR == presentRes || VK_SUBOPTIMAL_KHR == presentRes || VK_SUBOPTIMAL_KHR == acquireRes || aFramebufferResized )
		{
			aFramebufferResized = false;
			lut::recreate_swapchain( aWindow );
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

	bool framebufferResized = false;
	glfwSetWindowUserPointer( window.window, &framebufferResized );
	glfwSetFramebufferSizeCallback( window.window, framebuffer_resized_callback );

	auto commandPool = lut::create_command_pool( window, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT );
	auto commandBuffer = lut::alloc_command_buffer( window, commandPool.handle );

	auto imageAvailable = lut::create_semaphore( window.device );
	auto inFlight = lut::create_fence( window.device, VK_FENCE_CREATE_SIGNALED_BIT );

	std::vector<VkImageLayout> swapImageLayouts;
	std::vector<lut::Semaphore> renderFinished;
	reset_swapchain_frame_resources( window, swapImageLayouts, renderFinished );

	while( !glfwWindowShouldClose( window.window ) )
	{
		glfwPollEvents();
		draw_frame( window, commandBuffer, imageAvailable, renderFinished, inFlight, swapImageLayouts, framebufferResized );
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
