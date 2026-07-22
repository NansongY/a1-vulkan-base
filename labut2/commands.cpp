#include "commands.hpp"

#include "error.hpp"
#include "to_string.hpp"

// SOLUTION_TAGS: vulkan-(ex-[^1]|cw-.)

namespace labut2
{
	CommandPool create_command_pool( VulkanContext const& aContext, VkCommandPoolCreateFlags aFlags )
	{
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = aFlags;
		poolInfo.queueFamilyIndex = aContext.graphicsFamilyIndex;

		VkCommandPool pool = VK_NULL_HANDLE;
		if( auto const res = vkCreateCommandPool( aContext.device, &poolInfo, nullptr, &pool ); VK_SUCCESS != res )
		{
			throw Error( "Unable to create command pool\n"
				"vkCreateCommandPool() returned {}", to_string(res)
			);
		}

		return CommandPool( aContext.device, pool );

	}

	VkCommandBuffer alloc_command_buffer( VulkanContext const& aContext, VkCommandPool aCmdPool )
	{
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = aCmdPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		VkCommandBuffer buffer = VK_NULL_HANDLE;
		if( auto const res = vkAllocateCommandBuffers( aContext.device, &allocInfo, &buffer ); VK_SUCCESS != res )
		{
			throw Error( "Unable to allocate command buffer\n"
				"vkAllocateCommandBuffers() returned {}", to_string(res)
			);
		}

		return buffer;
	}
}
