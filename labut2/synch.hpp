#ifndef SYCH_HPP_38B52A38_F7ED_426D_9E10_D72C082648D5
#define SYCH_HPP_38B52A38_F7ED_426D_9E10_D72C082648D5
// SOLUTION_TAGS: vulkan-(ex-[^1]|cw-.)

#include <volk/volk.h>

#include <cstdint>

#include "vkobject.hpp"


namespace labut2
{
	Fence create_fence( VkDevice, VkFenceCreateFlags = 0 );
	Semaphore create_semaphore( VkDevice );


}

#endif // SYNCH_HPP_38B52A38_F7ED_426D_9E10_D72C082648D5
