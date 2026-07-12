#include "commands.hpp"

#include "error.hpp"
#include "to_string.hpp"

// SOLUTION_TAGS: vulkan-(ex-[^1]|cw-.)

namespace labut2
{
	CommandPool create_command_pool( VulkanContext const& aContext, VkCommandPoolCreateFlags aFlags )
	{
		throw Error( "Not yet implemented" ); //TODO: implement me!

	}

	VkCommandBuffer alloc_command_buffer( VulkanContext const& aContext, VkCommandPool aCmdPool )
	{
		throw Error( "Not yet implemented" ); //TODO: implement me!
	}
}
