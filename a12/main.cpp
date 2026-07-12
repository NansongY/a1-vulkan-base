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


	// Local types/structures:

}

int main() try
{
	//TODO-implement me.


	return 0;
}
catch( std::exception const& eErr )
{
	std::print( stderr, "\n" );
	std::print( stderr, "Error: {}\n", eErr.what() );
	return 1;
}


//EOF vim:syntax=cpp:foldmethod=marker:ts=4:noexpandtab: 
