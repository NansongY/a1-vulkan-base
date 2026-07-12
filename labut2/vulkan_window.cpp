#include "vulkan_window.hpp"

// SOLUTION_TAGS: vulkan-(ex-[^12]|cw-.)

#include <print>
#include <tuple>
#include <limits>
#include <vector>
#include <utility>
#include <optional>
#include <algorithm>
#include <unordered_set>

#include <cassert>

#include "error.hpp"
#include "to_string.hpp"
#include "context_helpers.hxx"
namespace lut = labut2;

namespace
{
	// The device selection process has changed somewhat w.r.t. the one used 
	// earlier (e.g., with VulkanContext.
	VkPhysicalDevice select_device( VkInstance, VkSurfaceKHR );
	float score_device( VkPhysicalDevice, VkSurfaceKHR );

	std::optional<std::uint32_t> find_queue_family( VkPhysicalDevice, VkQueueFlags, VkSurfaceKHR = VK_NULL_HANDLE );

	VkDevice create_device( 
		VkPhysicalDevice,
		std::vector<std::uint32_t> const& aQueueFamilies,
		std::vector<char const*> const& aEnabledDeviceExtensions = {}
	);

	std::vector<VkSurfaceFormatKHR> get_surface_formats( VkPhysicalDevice, VkSurfaceKHR );
	std::unordered_set<VkPresentModeKHR> get_present_modes( VkPhysicalDevice, VkSurfaceKHR );

	std::tuple<VkSwapchainKHR,VkFormat,VkExtent2D> create_swapchain(
		VkPhysicalDevice,
		VkSurfaceKHR,
		VkDevice,
		GLFWwindow*,
		std::vector<std::uint32_t> const& aQueueFamilyIndices = {},
		VkSwapchainKHR aOldSwapchain = VK_NULL_HANDLE
	);

	void get_swapchain_images( VkDevice, VkSwapchainKHR, std::vector<VkImage>& );
	void create_swapchain_image_views( VkDevice, VkFormat, std::vector<VkImage> const&, std::vector<VkImageView>& );
}

namespace labut2
{
	// VulkanWindow
	VulkanWindow::VulkanWindow() = default;

	VulkanWindow::~VulkanWindow()
	{
		// Device-related objects
		for( auto const view : swapViews )
			vkDestroyImageView( device, view, nullptr );

		if( VK_NULL_HANDLE != swapchain )
			vkDestroySwapchainKHR( device, swapchain, nullptr );

		// Window and related objects
		if( VK_NULL_HANDLE != surface )
			vkDestroySurfaceKHR( instance, surface, nullptr );

		if( window )
		{
			glfwDestroyWindow( window );

			// The following assumes that we never create more than one window;
			// if there are multiple windows, destroying one of them would
			// unload the whole GLFW library. Nevertheless, this solution is
			// convenient when only dealing with one window (which we will do
			// in the exercises), as it ensure that GLFW is unloaded after all
			// window-related resources are.
			glfwTerminate();
		}
	}

	VulkanWindow::VulkanWindow( VulkanWindow&& aOther ) noexcept
		: VulkanContext( std::move(aOther) )
		, window( std::exchange( aOther.window, VK_NULL_HANDLE ) )
		, surface( std::exchange( aOther.surface, VK_NULL_HANDLE ) )
		, presentFamilyIndex( aOther.presentFamilyIndex )
		, presentQueue( std::exchange( aOther.presentQueue, VK_NULL_HANDLE ) )
		, swapchain( std::exchange( aOther.swapchain, VK_NULL_HANDLE ) )
		, swapImages( std::move( aOther.swapImages ) )
		, swapViews( std::move( aOther.swapViews ) )
		, swapchainFormat( aOther.swapchainFormat )
		, swapchainExtent( aOther.swapchainExtent )
	{}

	VulkanWindow& VulkanWindow::operator=( VulkanWindow&& aOther ) noexcept
	{
		VulkanContext::operator=( std::move(aOther) );
		std::swap( window, aOther.window );
		std::swap( surface, aOther.surface );
		std::swap( presentFamilyIndex, aOther.presentFamilyIndex );
		std::swap( presentQueue, aOther.presentQueue );
		std::swap( swapchain, aOther.swapchain );
		std::swap( swapImages, aOther.swapImages );
		std::swap( swapViews, aOther.swapViews );
		std::swap( swapchainFormat, aOther.swapchainFormat );
		std::swap( swapchainExtent, aOther.swapchainExtent );
		return *this;
	}

	// make_vulkan_window()
	VulkanWindow make_vulkan_window()
	{
		VulkanWindow ret;

		// Initialize Volk
		if( auto const res = volkInitialize(); VK_SUCCESS != res )
		{
			throw Error( "Unable to load Vulkan API\n" 
				"Volk returned error {}", to_string(res)
			);
		}

		//TODO: initialize GLFW

		// Check for instance layers and extensions
		auto const supportedLayers = detail::get_instance_layers();
		auto const supportedExtensions = detail::get_instance_extensions();

		bool enableDebugUtils = false;

		std::vector<char const*> enabledLayers, enabledExensions;

		//TODO: check that the instance extensions required by GLFW are available,
		//TODO: and if so, request these to be enabled in the instance creation.

		// Validation layers support.
#		if !defined(NDEBUG) // debug builds only
		if( supportedLayers.count( "VK_LAYER_KHRONOS_validation" ) )
		{
			enabledLayers.emplace_back( "VK_LAYER_KHRONOS_validation" );
		}

		if( supportedExtensions.count( "VK_EXT_debug_utils" ) )
		{
			enableDebugUtils = true;
			enabledExensions.emplace_back( "VK_EXT_debug_utils" );
		}
#		endif // ~ debug builds

		for( auto const& layer : enabledLayers )
			std::print( stderr, "Enabling layer: {}\n", layer );

		for( auto const& extension : enabledExensions )
			std::print( stderr, "Enabling instance extension: {}\n", extension );

		// Create Vulkan instance
		ret.instance = detail::create_instance( enabledLayers, enabledExensions, enableDebugUtils );

		// Load rest of the Vulkan API
		volkLoadInstance( ret.instance );

		// Setup debug messenger
		if( enableDebugUtils )
			ret.debugMessenger = detail::create_debug_messenger( ret.instance );

		//TODO: create GLFW window
		//TODO: get VkSurfaceKHR from the window

		// Select appropriate Vulkan device
		ret.physicalDevice = select_device( ret.instance, ret.surface );
		if( VK_NULL_HANDLE == ret.physicalDevice )
			throw Error( "No suitable physical device found!" );

		{
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties( ret.physicalDevice, &props );
			std::print( stderr, "Selected device: {} ({}.{}.{})\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion) );
		}

		// Create a logical device
		// Enable required extensions. The device selection method ensures that
		// the VK_KHR_swapchain extension is present, so we can safely just
		// request it without further checks.
		std::vector<char const*> enabledDevExensions;

		//TODO: list necessary extensions here

		for( auto const& ext : enabledDevExensions )
			std::print( stderr, "Enabling device extension: {}\n", ext );

		// We need one or two queues:
		// - best case: one GRAPHICS queue that can present
		// - otherwise: one GRAPHICS queue and any queue that can present
		std::vector<std::uint32_t> queueFamilyIndices;

		//TODO: logic to select necessary queue families to instantiate

		ret.device = create_device( ret.physicalDevice, queueFamilyIndices, enabledDevExensions );

		// Retrieve VkQueues
		vkGetDeviceQueue( ret.device, ret.graphicsFamilyIndex, 0, &ret.graphicsQueue );

		assert( VK_NULL_HANDLE != ret.graphicsQueue );

		if( queueFamilyIndices.size() >= 2 )
			vkGetDeviceQueue( ret.device, ret.presentFamilyIndex, 0, &ret.presentQueue );
		else
		{
			ret.presentFamilyIndex = ret.graphicsFamilyIndex;
			ret.presentQueue = ret.graphicsQueue;
		}

		// Create swap chain
		std::tie(ret.swapchain, ret.swapchainFormat, ret.swapchainExtent) = create_swapchain( ret.physicalDevice, ret.surface, ret.device, ret.window, queueFamilyIndices );
		
		// Get swap chain images & create associated image views
		get_swapchain_images( ret.device, ret.swapchain, ret.swapImages );
		create_swapchain_image_views( ret.device, ret.swapchainFormat, ret.swapImages, ret.swapViews );

		// Done
		return ret;
	}

	SwapChanges recreate_swapchain( VulkanWindow& aWindow )
	{
		//TODO: implement me!
		throw Error( "Not yet implemented!" );
	}
}

namespace
{
	std::vector<VkSurfaceFormatKHR> get_surface_formats( VkPhysicalDevice aPhysicalDev, VkSurfaceKHR aSurface )
	{
		//TODO: implement me!
		throw lut::Error( "Not yet implemented!" );
	}

	std::unordered_set<VkPresentModeKHR> get_present_modes( VkPhysicalDevice aPhysicalDev, VkSurfaceKHR aSurface )
	{
		//TODO: implement me!
		throw lut::Error( "Not yet implemented!" );
	}

	std::tuple<VkSwapchainKHR,VkFormat,VkExtent2D> create_swapchain( VkPhysicalDevice aPhysicalDev, VkSurfaceKHR aSurface, VkDevice aDevice, GLFWwindow* aWindow, std::vector<std::uint32_t> const& aQueueFamilyIndices, VkSwapchainKHR aOldSwapchain )
	{
		auto const formats = get_surface_formats( aPhysicalDev, aSurface );
		auto const modes = get_present_modes( aPhysicalDev, aSurface );

		//TODO: pick appropriate VkSurfaceFormatKHR format.
		VkSurfaceFormatKHR format{}; // FIXME!

		//TODO: pick appropriate VkPresentModeKHR
		VkPresentModeKHR presentMode{}; // FIXME

		//TODO: pick image count
		std::uint32_t imageCount{}; // FIXME

		//TODO: figure out swap extent
		VkExtent2D extent{}; // FIXME

		// TODO: create swap chain
		throw lut::Error( "Not yet implemented!" );
	}


	void get_swapchain_images( VkDevice aDevice, VkSwapchainKHR aSwapchain, std::vector<VkImage>& aImages )
	{
		assert( 0 == aImages.size() );

		// TODO: get swapchain image handles with vkGetSwapchainImagesKHR
		throw lut::Error( "Not yet implemented!" );
	}

	void create_swapchain_image_views( VkDevice aDevice, VkFormat aSwapchainFormat, std::vector<VkImage> const& aImages, std::vector<VkImageView>& aViews )
	{
		assert( 0 == aViews.size() );

		// TODO: create a VkImageView for each of the VkImages.
		throw lut::Error( "Not yet implemented!" );

		assert( aViews.size() == aImages.size() );
	}
}

namespace
{
	// Note: this finds *any* queue that supports the aQueueFlags. As such,
	//   find_queue_family( ..., VK_QUEUE_TRANSFER_BIT, ... );
	// might return a GRAPHICS queue family, since GRAPHICS queues typically
	// also set TRANSFER (and indeed most other operations; GRAPHICS queues are
	// required to support those operations regardless). If you wanted to find
	// a dedicated TRANSFER queue (e.g., such as those that exist on NVIDIA
	// GPUs), you would need to use different logic.
	std::optional<std::uint32_t> find_queue_family( VkPhysicalDevice aPhysicalDev, VkQueueFlags aQueueFlags, VkSurfaceKHR aSurface )
	{
		//TODO: find queue family with the specified queue flags that can 
		//TODO: present to the surface (if specified)

		return {};
	}

	VkDevice create_device( VkPhysicalDevice aPhysicalDev, std::vector<std::uint32_t> const& aQueues, std::vector<char const*> const& aEnabledExtensions )
	{
		if( aQueues.empty() )
			throw lut::Error( "create_device(): no queues requested" );

		float queuePriorities[1] = { 1.f };

		std::vector<VkDeviceQueueCreateInfo> queueInfos( aQueues.size() );
		for( std::size_t i = 0; i < aQueues.size(); ++i )
		{
			auto& queueInfo = queueInfos[i];
			queueInfo.sType  = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueInfo.queueFamilyIndex  = aQueues[i];
			queueInfo.queueCount        = 1;
			queueInfo.pQueuePriorities  = queuePriorities;
		}

		VkPhysicalDeviceFeatures deviceFeatures{};
		// No extra Vulkan 1.0 features for now.

		VkPhysicalDeviceVulkan13Features vk13{};
		vk13.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vk13.synchronization2  = VK_TRUE;
		vk13.dynamicRendering  = VK_TRUE;

		VkPhysicalDeviceVulkan14Features vk14{};
		vk14.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
		vk14.pNext  = &vk13;
		vk14.maintenance5  = VK_TRUE; // Required in Vulkan 1.4, but we need to say that we want it.

		VkDeviceCreateInfo deviceInfo{};
		deviceInfo.sType  = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

		deviceInfo.queueCreateInfoCount     = std::uint32_t(queueInfos.size());
		deviceInfo.pQueueCreateInfos        = queueInfos.data();

		deviceInfo.enabledExtensionCount    = std::uint32_t(aEnabledExtensions.size());
		deviceInfo.ppEnabledExtensionNames  = aEnabledExtensions.data();

		deviceInfo.pEnabledFeatures         = &deviceFeatures;

		deviceInfo.pNext                    = &vk14;

		VkDevice device = VK_NULL_HANDLE;
		if( auto const res = vkCreateDevice( aPhysicalDev, &deviceInfo, nullptr, &device ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to create logical device\n"
				"vkCreateDevice() returned {}", lut::to_string(res)
			);
		}

		return device;
	}
}

namespace
{
	float score_device( VkPhysicalDevice aPhysicalDev, VkSurfaceKHR aSurface )
	{
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties( aPhysicalDev, &props );

		// Only consider Vulkan 1.4 devices ...
		auto const major = VK_API_VERSION_MAJOR( props.apiVersion );
		auto const minor = VK_API_VERSION_MINOR( props.apiVersion );

		if( major < 1 || (major == 1 && minor < 4) )
		{
			std::print( stderr, "Info: Discarding device '{}': insufficient vulkan version at {}.{}\n", props.deviceName, major, minor );
			return -1.f;
		}

		// ... with the required device features
		if( auto const missing = lut::detail::check_required_device_features( aPhysicalDev ); !missing.empty() )
		{
			std::print( stderr, "Info: Discarding device '{}': {} required features missing:\n", props.deviceName, missing.size() );
			for( auto const* feat : missing )
				std::print( "  - {}\n", feat );
			return -1.f;
		}

		//TODO: additional checks
		//TODO:  - check that the VK_KHR_swapchain extension is supported
		//TODO:  - check that there is a queue family that can present to the
		//TODO:    given surface
		//TODO:  - check that there is a queue family that supports graphics
		//TODO:    commands

		// Discrete GPU > Integrated GPU > others
		float score = 0.f;

		if( VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU == props.deviceType )
			score += 500.f;
		else if( VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU == props.deviceType )
			score += 100.f;

		return score;
	}
	
	VkPhysicalDevice select_device( VkInstance aInstance, VkSurfaceKHR aSurface )
	{
		std::uint32_t numDevices = 0;
		if( auto const res = vkEnumeratePhysicalDevices( aInstance, &numDevices, nullptr ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to get physical device count\n"
				"vkEnumeratePhysicalDevices() returned {}", lut::to_string(res)
			);
		}

		std::vector<VkPhysicalDevice> devices( numDevices, VK_NULL_HANDLE );
		if( auto const res = vkEnumeratePhysicalDevices( aInstance, &numDevices, devices.data() ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to get physical device list\n"
				"vkEnumeratePhysicalDevices() returned {}", lut::to_string(res)
			);
		}

		float bestScore = -1.f;
		VkPhysicalDevice bestDevice = VK_NULL_HANDLE;

		for( auto const device : devices )
		{
			auto const score = score_device( device, aSurface );
			if( score > bestScore )
			{
				bestScore = score;
				bestDevice = device;
			}
		}

		return bestDevice;
	}
}

