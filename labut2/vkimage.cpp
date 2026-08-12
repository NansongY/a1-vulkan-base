#include "vkimage.hpp"

#include <bit>
#include <print>
#include <limits>
#include <vector>
#include <memory>
#include <utility>
#include <algorithm>

#include <cassert>
#include <cstring> // for std::memcpy()

#include <stb_image.h>

#include "error.hpp"
#include "synch.hpp"
#include "commands.hpp"
#include "vkbuffer.hpp"
#include "to_string.hpp"

// SOLUTION_TAGS: vulkan-(ex-[^123]|cw-.)


namespace labut2
{
	Image::Image() noexcept = default;

	Image::~Image()
	{
		if( VK_NULL_HANDLE != image )
		{
			assert( VK_NULL_HANDLE != mAllocator );
			assert( VK_NULL_HANDLE != allocation );
			vmaDestroyImage( mAllocator, image, allocation );
		}
	}

	Image::Image( VmaAllocator aAllocator, VkImage aImage, VmaAllocation aAllocation ) noexcept
		: image( aImage )
		, allocation( aAllocation )
		, mAllocator( aAllocator )
	{}

	Image::Image( Image&& aOther ) noexcept
		: image( std::exchange( aOther.image, VK_NULL_HANDLE ) )
		, allocation( std::exchange( aOther.allocation, VK_NULL_HANDLE ) )
		, mAllocator( std::exchange( aOther.mAllocator, VK_NULL_HANDLE ) )
	{}
	Image& Image::operator=( Image&& aOther ) noexcept
	{
		std::swap( image, aOther.image );
		std::swap( allocation, aOther.allocation );
		std::swap( mAllocator, aOther.mAllocator );
		return *this;
	}


	ImageWithView::ImageWithView() noexcept = default;

	ImageWithView::~ImageWithView()
	{
		if( VK_NULL_HANDLE != view )
		{
			// This is a bit of a hack, but means we can just keep the
			// VmaAllocator handle, without also having to store a VkDevice
			// handle (which is indeed already stored in the allocator).
			assert( VK_NULL_HANDLE != mAllocator );

			VmaAllocatorInfo ainfo{};
			vmaGetAllocatorInfo( mAllocator, &ainfo );

			vkDestroyImageView( ainfo.device, view, nullptr );
		}
	}

	ImageWithView::ImageWithView( Image&& aImage, VkImageView aView ) noexcept
		: Image( std::move(aImage) )
		, view( aView )
	{}
	ImageWithView::ImageWithView( VmaAllocator aAllocator, VkImage aImage, VmaAllocation aAllocation, VkImageView aView ) noexcept
		: Image( aAllocator, aImage, aAllocation )
		, view( aView )
	{}

	ImageWithView::ImageWithView( ImageWithView&& aOther ) noexcept
		: Image( std::move(aOther) )
		, view( std::exchange( aOther.view, VK_NULL_HANDLE ) )
	{}

	ImageWithView& ImageWithView::operator= (ImageWithView&& aOther) noexcept
	{
		static_cast<Image&>(*this) = std::move(aOther);
		std::swap( view, aOther.view );
		return *this;
	}
}

namespace labut2
{
	Image create_image_texture2d( Allocator const& aAllocator, std::uint32_t aWidth, std::uint32_t aHeight, VkFormat aFormat, VkImageUsageFlags aUsage )
	{
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = aFormat;
		imageInfo.extent.width = aWidth;
		imageInfo.extent.height = aHeight;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = compute_mip_level_count( aWidth, aHeight );
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = aUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // needed for mipmap generation
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		if( auto const res = vmaCreateImage( aAllocator.allocator, &imageInfo, &allocInfo, &image, &allocation, nullptr ); VK_SUCCESS != res )
		{
			throw Error( "Unable to create texture image\n"
				"vmaCreateImage() returned {}", to_string(res)
			);
		}

		return Image( aAllocator.allocator, image, allocation );
	}

	Image load_image_texture2d( char const* aPath, VulkanContext const& aContext, VkCommandPool aCmdPool, Allocator const& aAllocator, VkFormat aFormat )
	{
		int width = 0, height = 0, channels = 0;
		stbi_uc* const pixels = stbi_load( aPath, &width, &height, &channels, 4 ); // force RGBA
		if( !pixels )
			throw Error( "Unable to load texture '{}'", aPath );

		// Make sure the pixel data is freed on every exit path.
		struct StbiFree { void operator()( stbi_uc* p ) const { stbi_image_free( p ); } };
		std::unique_ptr<stbi_uc, StbiFree> const pixelGuard( pixels );

		VkDeviceSize const imageSize = VkDeviceSize(width) * height * 4;
		VkFormat const format = aFormat;

		// 1) Upload pixels into a host-visible staging buffer.
		Buffer staging = create_buffer(
			aAllocator,
			imageSize,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
			VMA_MEMORY_USAGE_AUTO
		);
		{
			void* data = nullptr;
			if( auto const res = vmaMapMemory( aAllocator.allocator, staging.allocation, &data ); VK_SUCCESS != res )
				throw Error( "vmaMapMemory() returned {}", to_string(res) );

			std::memcpy( data, pixels, imageSize );
			vmaUnmapMemory( aAllocator.allocator, staging.allocation );
		}

		// 2) Create the GPU-side (device local) image with a full mip chain.
		Image image = create_image_texture2d(
			aAllocator,
			std::uint32_t(width),
			std::uint32_t(height),
			format,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
		);

		// 3) Record: copy buffer->image, then generate mipmaps with blits.
		VkCommandBuffer cmd = alloc_command_buffer( aContext, aCmdPool );

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if( auto const res = vkBeginCommandBuffer( cmd, &beginInfo ); VK_SUCCESS != res )
			throw Error( "vkBeginCommandBuffer() returned {}", to_string(res) );

		// Small local helper for image layout transitions (synchronization2).
		auto const imageBarrier = [&]( VkCommandBuffer aCmd, VkImageLayout aOld, VkImageLayout aNew, VkPipelineStageFlags2 aSrcStage, VkAccessFlags2 aSrcAccess, VkPipelineStageFlags2 aDstStage, VkAccessFlags2 aDstAccess, std::uint32_t aBaseMip, std::uint32_t aLevelCount )
		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask = aSrcStage;
			barrier.srcAccessMask = aSrcAccess;
			barrier.dstStageMask = aDstStage;
			barrier.dstAccessMask = aDstAccess;
			barrier.oldLayout = aOld;
			barrier.newLayout = aNew;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image.image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseMipLevel = aBaseMip;
			barrier.subresourceRange.levelCount = aLevelCount;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = 1;

			VkDependencyInfo dep{};
			dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers = &barrier;
			vkCmdPipelineBarrier2( aCmd, &dep );
		};

		// How many mip levels this texture has.
		std::uint32_t const mipLevels = compute_mip_level_count( std::uint32_t(width), std::uint32_t(height) );

		// ALL levels: UNDEFINED -> TRANSFER_DST. Every level must be in
		// TRANSFER_DST before the copy/blits write into it.
		imageBarrier( cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			0, mipLevels );

		// Copy the staging buffer into mip level 0.
		VkBufferImageCopy region{};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = { std::uint32_t(width), std::uint32_t(height), 1 };
		vkCmdCopyBufferToImage( cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

		// Generate mipmaps: blit each level down to the next smaller one.
		int mipWidth = width, mipHeight = height;
		for( std::uint32_t mip = 1; mip < mipLevels; ++mip )
		{
			// Previous level: TRANSFER_DST -> TRANSFER_SRC (so it can be read by the blit).
			imageBarrier( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
				mip-1, 1 );

			VkImageBlit blit{};
			blit.srcOffsets[0] = { 0, 0, 0 };
			blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
			blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.mipLevel = mip - 1;
			blit.srcSubresource.layerCount = 1;
			blit.dstOffsets[0] = { 0, 0, 0 };
			blit.dstOffsets[1] = { std::max(mipWidth/2, 1), std::max(mipHeight/2, 1), 1 };
			blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.dstSubresource.mipLevel = mip;
			blit.dstSubresource.layerCount = 1;
			vkCmdBlitImage( cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR );

			mipWidth = std::max( mipWidth/2, 1 );
			mipHeight = std::max( mipHeight/2, 1 );
		}

		// Final: every level -> SHADER_READ_ONLY.
		// Levels 0..mipLevels-2 are in TRANSFER_SRC, the last level is in TRANSFER_DST.
		if( mipLevels > 1 )
		{
			imageBarrier( cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
				0, mipLevels-1 );
			imageBarrier( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
				mipLevels-1, 1 );
		}
		else
		{
			imageBarrier( cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
				0, 1 );
		}

		if( auto const res = vkEndCommandBuffer( cmd ); VK_SUCCESS != res )
			throw Error( "vkEndCommandBuffer() returned {}", to_string(res) );

		// 4) Submit and wait for completion.
		VkCommandBufferSubmitInfo cmdInfo{};
		cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
		cmdInfo.commandBuffer = cmd;

		VkSubmitInfo2 submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
		submitInfo.commandBufferInfoCount = 1;
		submitInfo.pCommandBufferInfos = &cmdInfo;

		auto fence = create_fence( aContext.device );
		if( auto const res = vkQueueSubmit2( aContext.graphicsQueue, 1, &submitInfo, fence.handle ); VK_SUCCESS != res )
			throw Error( "vkQueueSubmit2() returned {}", to_string(res) );
		if( auto const res = vkWaitForFences( aContext.device, 1, &fence.handle, VK_TRUE, 1000000000ULL*120ULL ); VK_SUCCESS != res )
			throw Error( "vkWaitForFences() returned {}", to_string(res) );

		return image;
	}

	std::uint32_t compute_mip_level_count( std::uint32_t aWidth, std::uint32_t aHeight )
	{
		std::uint32_t const bits = aWidth | aHeight;
		std::uint32_t const leadingZeros = std::countl_zero( bits );
		return 32-leadingZeros;
	}
}
