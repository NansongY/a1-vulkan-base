#include "vkbuffer.hpp"

// SOLUTION_TAGS: vulkan-(ex-[^123]|cw-.)

#include <utility>

#include <cassert>

#include "error.hpp"
#include "to_string.hpp"


namespace labut2
{
	Buffer::Buffer() noexcept = default;

	Buffer::~Buffer()
	{
		if( VK_NULL_HANDLE != buffer )
		{
			assert( VK_NULL_HANDLE != mAllocator );
			assert( VK_NULL_HANDLE != allocation );
			vmaDestroyBuffer( mAllocator, buffer, allocation );
		}
	}

	Buffer::Buffer( VmaAllocator aAllocator, VkBuffer aBuffer, VmaAllocation aAllocation ) noexcept
		: buffer( aBuffer )
		, allocation( aAllocation )
		, mAllocator( aAllocator )
	{}

	Buffer::Buffer( Buffer&& aOther ) noexcept
		: buffer( std::exchange( aOther.buffer, VK_NULL_HANDLE ) )
		, allocation( std::exchange( aOther.allocation, VK_NULL_HANDLE ) )
		, mAllocator( std::exchange( aOther.mAllocator, VK_NULL_HANDLE ) )
	{}
	Buffer& Buffer::operator=( Buffer&& aOther ) noexcept
	{
		std::swap( buffer, aOther.buffer );
		std::swap( allocation, aOther.allocation );
		std::swap( mAllocator, aOther.mAllocator );
		return *this;
	}
}

namespace labut2
{
	Buffer create_buffer( Allocator const& aAllocator, VkDeviceSize aSize, VkBufferUsageFlags aBufferUsage, VmaAllocationCreateFlags aMemoryFlags, VmaMemoryUsage aMemoryUsage )
	{
		throw Error( "Not yet implemented" ); //TODO- implement me!
	}
}
