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

		if( !glfwInit() )
			throw Error( "Unable to initialize GLFW" );

		if( !glfwVulkanSupported() )
		{
			glfwTerminate();
			throw Error( "GLFW reports that Vulkan is not supported" );
		}

		glfwWindowHint( GLFW_CLIENT_API, GLFW_NO_API );
		glfwWindowHint( GLFW_RESIZABLE, GLFW_TRUE );

		ret.window = glfwCreateWindow( 1280, 720, "COMP5892M A12", nullptr, nullptr );
		if( !ret.window )
		{
			glfwTerminate();
			throw Error( "Unable to create GLFW window" );
		}

		// Check for instance layers and extensions
		auto const supportedLayers = detail::get_instance_layers();
		auto const supportedExtensions = detail::get_instance_extensions();

		bool enableDebugUtils = false;

		std::vector<char const*> enabledLayers, enabledExensions;

		std::uint32_t glfwExtensionCount = 0;
		auto const glfwExtensions = glfwGetRequiredInstanceExtensions( &glfwExtensionCount );
		if( !glfwExtensions || 0 == glfwExtensionCount )
			throw Error( "GLFW did not report any required Vulkan instance extensions" );

		for( std::uint32_t i = 0; i < glfwExtensionCount; ++i )
		{
			if( !supportedExtensions.count( glfwExtensions[i] ) )
				throw Error( "Required GLFW instance extension '{}' is not supported", glfwExtensions[i] );

			enabledExensions.emplace_back( glfwExtensions[i] );
		}

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

		if( auto const res = glfwCreateWindowSurface( ret.instance, ret.window, nullptr, &ret.surface ); VK_SUCCESS != res )
		{
			throw Error( "Unable to create Vulkan surface\n"
				"glfwCreateWindowSurface() returned {}", to_string(res)
			);
		}

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

		enabledDevExensions.emplace_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );

		for( auto const& ext : enabledDevExensions )
			std::print( stderr, "Enabling device extension: {}\n", ext );

		// We need one or two queues:
		// - best case: one GRAPHICS queue that can present
		// - otherwise: one GRAPHICS queue and any queue that can present
		std::vector<std::uint32_t> queueFamilyIndices;

		if( auto const graphicsAndPresent = find_queue_family( ret.physicalDevice, VK_QUEUE_GRAPHICS_BIT, ret.surface ) )
		{
			ret.graphicsFamilyIndex = *graphicsAndPresent;
			ret.presentFamilyIndex = *graphicsAndPresent;
			queueFamilyIndices.emplace_back( *graphicsAndPresent );
		}
		else
		{
			auto const graphics = find_queue_family( ret.physicalDevice, VK_QUEUE_GRAPHICS_BIT );
			auto const present = find_queue_family( ret.physicalDevice, 0, ret.surface );

			if( !graphics )
				throw Error( "No queue family with GRAPHICS" );

			if( !present )
				throw Error( "No queue family that can present to the selected surface" );

			ret.graphicsFamilyIndex = *graphics;
			ret.presentFamilyIndex = *present;

			queueFamilyIndices.emplace_back( ret.graphicsFamilyIndex );
			if( ret.presentFamilyIndex != ret.graphicsFamilyIndex )
				queueFamilyIndices.emplace_back( ret.presentFamilyIndex );
		}

		ret.device = create_device( ret.physicalDevice, queueFamilyIndices, enabledDevExensions );
		volkLoadDevice( ret.device );

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
		int width = 0, height = 0;
		glfwGetFramebufferSize( aWindow.window, &width, &height );
		while( 0 == width || 0 == height )
		{
			glfwWaitEvents();
			glfwGetFramebufferSize( aWindow.window, &width, &height );
		}

		if( auto const res = vkDeviceWaitIdle( aWindow.device ); VK_SUCCESS != res )
		{
			throw Error( "Unable to wait for device before recreating swapchain\n"
				"vkDeviceWaitIdle() returned {}", to_string(res)
			);
		}

		auto const oldSwapchain = aWindow.swapchain;
		auto const oldFormat = aWindow.swapchainFormat;
		auto const oldExtent = aWindow.swapchainExtent;

		for( auto const view : aWindow.swapViews )
			vkDestroyImageView( aWindow.device, view, nullptr );

		aWindow.swapViews.clear();
		aWindow.swapImages.clear();

		std::tie( aWindow.swapchain, aWindow.swapchainFormat, aWindow.swapchainExtent ) = create_swapchain(
			aWindow.physicalDevice,
			aWindow.surface,
			aWindow.device,
			aWindow.window,
			{ aWindow.graphicsFamilyIndex, aWindow.presentFamilyIndex },
			oldSwapchain
		);

		if( VK_NULL_HANDLE != oldSwapchain )
			vkDestroySwapchainKHR( aWindow.device, oldSwapchain, nullptr );

		get_swapchain_images( aWindow.device, aWindow.swapchain, aWindow.swapImages );
		create_swapchain_image_views( aWindow.device, aWindow.swapchainFormat, aWindow.swapImages, aWindow.swapViews );

		SwapChanges changes{};
		changes.changedFormat = oldFormat != aWindow.swapchainFormat;
		changes.changedSize = oldExtent.width != aWindow.swapchainExtent.width || oldExtent.height != aWindow.swapchainExtent.height;
		return changes;
	}
}

namespace
{
	std::vector<VkSurfaceFormatKHR> get_surface_formats( VkPhysicalDevice aPhysicalDev, VkSurfaceKHR aSurface )
	{
		std::uint32_t count = 0;
		if( auto const res = vkGetPhysicalDeviceSurfaceFormatsKHR( aPhysicalDev, aSurface, &count, nullptr ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to get surface format count\n"
				"vkGetPhysicalDeviceSurfaceFormatsKHR() returned {}", lut::to_string(res)
			);
		}

		std::vector<VkSurfaceFormatKHR> formats( count );
		if( count )
		{
			if( auto const res = vkGetPhysicalDeviceSurfaceFormatsKHR( aPhysicalDev, aSurface, &count, formats.data() ); VK_SUCCESS != res )
			{
				throw lut::Error( "Unable to get surface formats\n"
					"vkGetPhysicalDeviceSurfaceFormatsKHR() returned {}", lut::to_string(res)
				);
			}
		}

		return formats;
	}

	std::unordered_set<VkPresentModeKHR> get_present_modes( VkPhysicalDevice aPhysicalDev, VkSurfaceKHR aSurface )
	{
		std::uint32_t count = 0;
		if( auto const res = vkGetPhysicalDeviceSurfacePresentModesKHR( aPhysicalDev, aSurface, &count, nullptr ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to get present mode count\n"
				"vkGetPhysicalDeviceSurfacePresentModesKHR() returned {}", lut::to_string(res)
			);
		}

		std::vector<VkPresentModeKHR> modes( count );
		if( count )
		{
			if( auto const res = vkGetPhysicalDeviceSurfacePresentModesKHR( aPhysicalDev, aSurface, &count, modes.data() ); VK_SUCCESS != res )
			{
				throw lut::Error( "Unable to get present modes\n"
					"vkGetPhysicalDeviceSurfacePresentModesKHR() returned {}", lut::to_string(res)
				);
			}
		}

		return { modes.begin(), modes.end() };
	}

	std::tuple<VkSwapchainKHR,VkFormat,VkExtent2D> create_swapchain( VkPhysicalDevice aPhysicalDev, VkSurfaceKHR aSurface, VkDevice aDevice, GLFWwindow* aWindow, std::vector<std::uint32_t> const& aQueueFamilyIndices, VkSwapchainKHR aOldSwapchain )
	{
		auto const formats = get_surface_formats( aPhysicalDev, aSurface );
		auto const modes = get_present_modes( aPhysicalDev, aSurface );

		if( formats.empty() )
			throw lut::Error( "Selected device/surface has no surface formats" );

		VkSurfaceFormatKHR format = formats[0];
		for( auto const& candidate : formats )
		{
			if( VK_COLOR_SPACE_SRGB_NONLINEAR_KHR == candidate.colorSpace &&
				( VK_FORMAT_B8G8R8A8_SRGB == candidate.format || VK_FORMAT_R8G8B8A8_SRGB == candidate.format ) )
			{
				format = candidate;
				break;
			}
		}

		if( !modes.count( VK_PRESENT_MODE_FIFO_KHR ) )
			throw lut::Error( "Selected device/surface does not support FIFO present mode" );

		VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

		VkSurfaceCapabilitiesKHR caps{};
		if( auto const res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR( aPhysicalDev, aSurface, &caps ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to get surface capabilities\n"
				"vkGetPhysicalDeviceSurfaceCapabilitiesKHR() returned {}", lut::to_string(res)
			);
		}

		std::uint32_t imageCount = caps.minImageCount + 1;
		if( 0 != caps.maxImageCount )
			imageCount = std::min( imageCount, caps.maxImageCount );

		VkExtent2D extent{};
		if( std::numeric_limits<std::uint32_t>::max() != caps.currentExtent.width )
		{
			extent = caps.currentExtent;
		}
		else
		{
			int width = 0, height = 0;
			glfwGetFramebufferSize( aWindow, &width, &height );

			extent.width = std::clamp( static_cast<std::uint32_t>( std::max( width, 1 ) ), caps.minImageExtent.width, caps.maxImageExtent.width );
			extent.height = std::clamp( static_cast<std::uint32_t>( std::max( height, 1 ) ), caps.minImageExtent.height, caps.maxImageExtent.height );
		}

		VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		if( !( caps.supportedCompositeAlpha & compositeAlpha ) )
		{
			for( auto const candidate : { VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR } )
			{
				if( caps.supportedCompositeAlpha & candidate )
				{
					compositeAlpha = candidate;
					break;
				}
			}
		}

		VkSwapchainCreateInfoKHR swapInfo{};
		swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		swapInfo.surface = aSurface;
		swapInfo.minImageCount = imageCount;
		swapInfo.imageFormat = format.format;
		swapInfo.imageColorSpace = format.colorSpace;
		swapInfo.imageExtent = extent;
		swapInfo.imageArrayLayers = 1;
		swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		swapInfo.preTransform = caps.currentTransform;
		swapInfo.compositeAlpha = compositeAlpha;
		swapInfo.presentMode = presentMode;
		swapInfo.clipped = VK_TRUE;
		swapInfo.oldSwapchain = aOldSwapchain;

		if( 2 <= aQueueFamilyIndices.size() && aQueueFamilyIndices[0] != aQueueFamilyIndices[1] )
		{
			swapInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			swapInfo.queueFamilyIndexCount = std::uint32_t( aQueueFamilyIndices.size() );
			swapInfo.pQueueFamilyIndices = aQueueFamilyIndices.data();
		}
		else
		{
			swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		if( auto const res = vkCreateSwapchainKHR( aDevice, &swapInfo, nullptr, &swapchain ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to create swapchain\n"
				"vkCreateSwapchainKHR() returned {}", lut::to_string(res)
			);
		}

		return { swapchain, format.format, extent };
	}


	void get_swapchain_images( VkDevice aDevice, VkSwapchainKHR aSwapchain, std::vector<VkImage>& aImages )
	{
		assert( 0 == aImages.size() );

		std::uint32_t count = 0;
		if( auto const res = vkGetSwapchainImagesKHR( aDevice, aSwapchain, &count, nullptr ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to get swapchain image count\n"
				"vkGetSwapchainImagesKHR() returned {}", lut::to_string(res)
			);
		}

		aImages.resize( count );
		if( auto const res = vkGetSwapchainImagesKHR( aDevice, aSwapchain, &count, aImages.data() ); VK_SUCCESS != res )
		{
			throw lut::Error( "Unable to get swapchain images\n"
				"vkGetSwapchainImagesKHR() returned {}", lut::to_string(res)
			);
		}
	}

	void create_swapchain_image_views( VkDevice aDevice, VkFormat aSwapchainFormat, std::vector<VkImage> const& aImages, std::vector<VkImageView>& aViews )
	{
		assert( 0 == aViews.size() );

		aViews.reserve( aImages.size() );
		for( auto const image : aImages )
		{
			VkImageViewCreateInfo viewInfo{};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = image;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = aSwapchainFormat;
			viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
			viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
			viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
			viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
			viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			viewInfo.subresourceRange.baseMipLevel = 0;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.baseArrayLayer = 0;
			viewInfo.subresourceRange.layerCount = 1;

			VkImageView view = VK_NULL_HANDLE;
			if( auto const res = vkCreateImageView( aDevice, &viewInfo, nullptr, &view ); VK_SUCCESS != res )
			{
				throw lut::Error( "Unable to create swapchain image view\n"
					"vkCreateImageView() returned {}", lut::to_string(res)
				);
			}

			aViews.emplace_back( view );
		}

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
		std::uint32_t numQueues = 0;
		vkGetPhysicalDeviceQueueFamilyProperties( aPhysicalDev, &numQueues, nullptr );

		std::vector<VkQueueFamilyProperties> families( numQueues );
		vkGetPhysicalDeviceQueueFamilyProperties( aPhysicalDev, &numQueues, families.data() );

		for( std::uint32_t i = 0; i < numQueues; ++i )
		{
			auto const& family = families[i];
			if( 0 == family.queueCount )
				continue;

			if( aQueueFlags != ( family.queueFlags & aQueueFlags ) )
				continue;

			if( VK_NULL_HANDLE != aSurface )
			{
				VkBool32 canPresent = VK_FALSE;
				if( auto const res = vkGetPhysicalDeviceSurfaceSupportKHR( aPhysicalDev, i, aSurface, &canPresent ); VK_SUCCESS != res )
				{
					throw lut::Error( "Unable to query queue family presentation support\n"
						"vkGetPhysicalDeviceSurfaceSupportKHR() returned {}", lut::to_string(res)
					);
				}

				if( !canPresent )
					continue;
			}

			return i;
		}

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
		if( auto const extensions = lut::detail::get_device_extensions( aPhysicalDev ); !extensions.count( VK_KHR_SWAPCHAIN_EXTENSION_NAME ) )
		{
			std::print( stderr, "Info: Discarding device '{}': missing {}\n", props.deviceName, VK_KHR_SWAPCHAIN_EXTENSION_NAME );
			return -1.f;
		}

		if( !find_queue_family( aPhysicalDev, VK_QUEUE_GRAPHICS_BIT ) )
		{
			std::print( stderr, "Info: Discarding device '{}': no graphics queue family\n", props.deviceName );
			return -1.f;
		}

		if( !find_queue_family( aPhysicalDev, 0, aSurface ) )
		{
			std::print( stderr, "Info: Discarding device '{}': no present queue family\n", props.deviceName );
			return -1.f;
		}

		if( get_surface_formats( aPhysicalDev, aSurface ).empty() || get_present_modes( aPhysicalDev, aSurface ).empty() )
		{
			std::print( stderr, "Info: Discarding device '{}': incomplete swapchain support\n", props.deviceName );
			return -1.f;
		}

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

