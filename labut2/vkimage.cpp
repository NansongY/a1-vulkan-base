#include "vkimage.hpp"

#include <bit>
#include <print>
#include <limits>
#include <vector>
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
	Image load_image_texture2d( char const* aPath, VulkanContext const& aContext, VkCommandPool aCmdPool, Allocator const& aAllocator )
	{
		throw Error( "Not yet implemented" ); //TODO- implement me!
	}

	Image create_image_texture2d( Allocator const& aAllocator, std::uint32_t aWidth, std::uint32_t aHeight, VkFormat aFormat, VkImageUsageFlags aUsage )
	{
		throw Error( "Not yet implemented" ); //TODO- implement me!
	}

	std::uint32_t compute_mip_level_count( std::uint32_t aWidth, std::uint32_t aHeight )
	{
		std::uint32_t const bits = aWidth | aHeight;
		std::uint32_t const leadingZeros = std::countl_zero( bits );
		return 32-leadingZeros;
	}
}
