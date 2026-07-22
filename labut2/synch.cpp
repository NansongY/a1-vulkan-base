#include "synch.hpp"

#include <cassert>

#include "error.hpp"
#include "to_string.hpp"

// SOLUTION_TAGS: vulkan-(ex-[^1]|cw-.)

namespace labut2
{
	Fence create_fence( VkDevice aDevice, VkFenceCreateFlags aFlags )
	{
		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = aFlags;

		VkFence fence = VK_NULL_HANDLE;
		if( auto const res = vkCreateFence( aDevice, &fenceInfo, nullptr, &fence ); VK_SUCCESS != res )
		{
			throw Error( "Unable to create fence\n"
				"vkCreateFence() returned {}", to_string(res)
			);
		}

		return Fence( aDevice, fence );
	}

	Semaphore create_semaphore( VkDevice aDevice )
	{
		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkSemaphore semaphore = VK_NULL_HANDLE;
		if( auto const res = vkCreateSemaphore( aDevice, &semaphoreInfo, nullptr, &semaphore ); VK_SUCCESS != res )
		{
			throw Error( "Unable to create semaphore\n"
				"vkCreateSemaphore() returned {}", to_string(res)
			);
		}

		return Semaphore( aDevice, semaphore );
	}


}
