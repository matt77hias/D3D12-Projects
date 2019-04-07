//-----------------------------------------------------------------------------
// Includes
//-----------------------------------------------------------------------------
#pragma region

#include <SDKDDKVer.h>
#include "exception\exception.hpp"
#include "logging\dump.hpp"
#include "ui\window.hpp"

#pragma endregion

//-----------------------------------------------------------------------------
// System Includes
//-----------------------------------------------------------------------------
#pragma region

#include <d3d12.h>
#include <dxgi1_4.h>

#pragma endregion

//-----------------------------------------------------------------------------
// Linker Directives
//-----------------------------------------------------------------------------
#pragma region

#pragma comment (lib, "d3d12.lib")
#pragma comment (lib, "dxgi.lib")

#pragma endregion

//-----------------------------------------------------------------------------
// Integrated + Dedicated GPU on notebooks
//-----------------------------------------------------------------------------
#pragma region

/**
 NVIDIA Optimus enablement
 @pre NVIDIA Control Panel > Preferred graphics processor > "Auto-select"
 */
extern "C" {
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

/**
 AMD "Optimus" enablement
 */
extern "C" {
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#pragma endregion

//-----------------------------------------------------------------------------
// Declarations and Definitions
//-----------------------------------------------------------------------------
using namespace mage;

namespace {

	constexpr U32x2 g_display_resolution = { 800u, 600u };
	UniquePtr< Window > g_window;

	constexpr D3D_FEATURE_LEVEL g_feature_level = D3D_FEATURE_LEVEL_11_1;

	ComPtr< ID3D12Device > g_device;
	ComPtr< ID3D12CommandQueue > g_command_queue;
	ComPtr< ID3D12CommandAllocator > g_command_allocator;
	ComPtr< ID3D12GraphicsCommandList > g_command_list;
	ComPtr< ID3D12Fence > g_fence;
	U64 g_fence_value = 0u;

	ComPtr< IDXGIFactory4 > g_factory;
	ComPtr< IDXGISwapChain1 > g_swap_chain;
	constexpr U32 g_nb_back_buffers = 2u;
	U32 g_back_buffer_index = 0u;

	ComPtr< ID3D12Resource > g_back_buffers[g_nb_back_buffers];
	ComPtr< ID3D12Resource > g_depth_stencil_buffer;

	ComPtr< ID3D12DescriptorHeap > g_rtv_heap;
	ComPtr< ID3D12DescriptorHeap > g_dsv_heap;
	U32 g_rtv_desc_size;
	U32 g_dsv_desc_size;

	D3D12_VIEWPORT g_viewport;
	D3D12_RECT g_scissor_rectangle;

	void FlushCommandQueue() {
		++g_fence_value;

		ThrowIfFailed(g_command_queue->Signal(g_fence.Get(), g_fence_value));

		if (g_fence->GetCompletedValue() < g_fence_value) {
			const HANDLE event_handle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);

			ThrowIfFailed(g_fence->SetEventOnCompletion(g_fence_value, event_handle));

			WaitForSingleObject(event_handle, INFINITE);
			CloseHandle(event_handle);
		}
	}

	void Init(NotNull< HINSTANCE > instance) {
		#ifdef _DEBUG
		const int debug_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
		// Perform automatic leak checking at program exit through a call to
		// _CrtDumpMemoryLeaks and generate an error report if the application
		// failed to free all the memory it allocated.
		_CrtSetDbgFlag(debug_flags | _CRTDBG_LEAK_CHECK_DF);
		#endif

		// Add filter for unhandled exceptions.
		AddUnhandledExceptionFilter();

		// Initialize a console.
		InitializeConsole();
		Print("Copyright (c) 2016-2019 Matthias Moulin.\n");

		// Initialize a window.
		{
			auto window_desc = MakeShared< WindowDescriptor >(instance, L"Demo");
			g_window = MakeUnique< Window >(std::move(window_desc),
											L"Demo",
											g_display_resolution);
		}

		#ifdef _DEBUG
		// Enable the debug layer.
		{
			ComPtr< ID3D12Debug > debug_controller;
			const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(debug_controller.ReleaseAndGetAddressOf()));
			ThrowIfFailed(result, "ID3D12Debug creation failed: {:08X}.", result);
			debug_controller->EnableDebugLayer();
		}
		#endif // _DEBUG

		// Initialize the device.
		{
			const HRESULT result = D3D12CreateDevice(nullptr,
													 g_feature_level,
													 IID_PPV_ARGS(g_device.ReleaseAndGetAddressOf()));
			ThrowIfFailed(result, "ID3D11Device creation failed: {:08X}.", result);
		}

		// Initialize the fence.
		{
			const HRESULT result = g_device->CreateFence(0u,
														 D3D12_FENCE_FLAG_NONE,
														 IID_PPV_ARGS(g_fence.ReleaseAndGetAddressOf()));
			ThrowIfFailed(result, "ID3D12Fence creation failed: {:08X}.", result);
		}

		// Initialize the command objects.
		{
			constexpr D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			
			// Create the command queue.
			{
				D3D12_COMMAND_QUEUE_DESC queue_desc = {};
				queue_desc.Type = type;

				const HRESULT result = g_device->CreateCommandQueue(&queue_desc,
																	IID_PPV_ARGS(g_command_queue.ReleaseAndGetAddressOf()));
				ThrowIfFailed(result, "ID3D12CommandQueue creation failed: {:08X}.", result);
			}

			// Create the command allocator.
			{
				const HRESULT result = g_device->CreateCommandAllocator(type,
																		IID_PPV_ARGS(g_command_allocator.ReleaseAndGetAddressOf()));
				ThrowIfFailed(result, "ID3D12CommandAllocator creation failed: {:08X}.", result);
			}

			// Create the command list.
			{
				const HRESULT result = g_device->CreateCommandList(0u,
																   type,
																   g_command_allocator.Get(),
																   nullptr,
																   IID_PPV_ARGS(g_command_list.ReleaseAndGetAddressOf()));
				ThrowIfFailed(result, "ID3D12CommandList creation failed: {:08X}.", result);
			}
		}

		// Initialize the swap chain.
		{
			// Get the IDXGIFactory4
			{
				const HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(g_factory.ReleaseAndGetAddressOf()));
				ThrowIfFailed(result, "IDXGIFactory2 creation failed: {:08X}.", result);
			}

			g_factory->MakeWindowAssociation(g_window->GetWindow(),
											   DXGI_MWA_NO_WINDOW_CHANGES
											 | DXGI_MWA_NO_ALT_ENTER
											 | DXGI_MWA_NO_PRINT_SCREEN);

			// Create the swap chain descriptor.
			DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
			swap_chain_desc.Width       = g_display_resolution[0u];
			swap_chain_desc.Height      = g_display_resolution[1u];
			swap_chain_desc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
			swap_chain_desc.SampleDesc.Count = 1u;
			swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			swap_chain_desc.BufferCount = g_nb_back_buffers;
			swap_chain_desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			swap_chain_desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
			swap_chain_desc.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

			// Create the swap chain.
			{
				const HRESULT result = g_factory->CreateSwapChainForHwnd(g_command_queue.Get(),
																		 g_window->GetWindow(),
																		 &swap_chain_desc,
																		 nullptr,
																		 nullptr,
																		 g_swap_chain.ReleaseAndGetAddressOf());
				ThrowIfFailed(result, "IDXGISwapChain creation failed: {:08X}.", result);
			}

			g_swap_chain->SetFullscreenState(FALSE, nullptr);
		}

		// Initialize the RTV heap.
		{
			constexpr D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			
			g_rtv_desc_size = g_device->GetDescriptorHandleIncrementSize(type);
			
			D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
			rtv_heap_desc.Type           = type;
			rtv_heap_desc.NumDescriptors = g_nb_back_buffers;

			const HRESULT result = g_device->CreateDescriptorHeap(&rtv_heap_desc,
																  IID_PPV_ARGS(g_rtv_heap.ReleaseAndGetAddressOf()));
			ThrowIfFailed(result, "ID3D12DescriptorHeap creation failed: {:08X}.", result);
		}

		// Initialize the DSV heap.
		{
			constexpr D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			
			g_dsv_desc_size = g_device->GetDescriptorHandleIncrementSize(type);
			
			D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {};
			dsv_heap_desc.Type           = type;
			dsv_heap_desc.NumDescriptors = 1u;
		
			const HRESULT result = g_device->CreateDescriptorHeap(&dsv_heap_desc,
																  IID_PPV_ARGS(g_dsv_heap.ReleaseAndGetAddressOf()));
			ThrowIfFailed(result, "ID3D12DescriptorHeap creation failed: {:08X}.", result);
		}

		// Initialize the RTVs.
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtv_desc_handle = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
			for (U32 i = 0u; i < g_nb_back_buffers; ++i) {
				
				const HRESULT result = g_swap_chain->GetBuffer(i, IID_PPV_ARGS(g_back_buffers[i].ReleaseAndGetAddressOf()));
				ThrowIfFailed(result, "ID3D12Resource retrieval failed: {:08X}.", result);

				g_device->CreateRenderTargetView(g_back_buffers[i].Get(), nullptr, rtv_desc_handle);

				rtv_desc_handle.ptr += g_rtv_desc_size;
			}
		}
		
		// Initialize the depth stencil buffer.
		{
			constexpr DXGI_FORMAT format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			
			D3D12_HEAP_PROPERTIES heap_properties = {};
			heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
			heap_properties.CreationNodeMask = 1u;
			heap_properties.VisibleNodeMask  = 1u;

			D3D12_RESOURCE_DESC depth_stencil_desc = {};
			depth_stencil_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			depth_stencil_desc.Width     = g_display_resolution[0u];
			depth_stencil_desc.Height    = g_display_resolution[1u];
			depth_stencil_desc.DepthOrArraySize = 1u;
			depth_stencil_desc.MipLevels = 1u;
			depth_stencil_desc.Format    = format;
			depth_stencil_desc.SampleDesc.Count = 1u;
			depth_stencil_desc.Flags     = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			D3D12_CLEAR_VALUE clear_value;
			clear_value.Format = format;
			clear_value.DepthStencil.Depth   = 1.0f;
			clear_value.DepthStencil.Stencil = 0u;

			const HRESULT result = g_device->CreateCommittedResource(&heap_properties,
																	 D3D12_HEAP_FLAG_NONE,
																	 &depth_stencil_desc,
																	 D3D12_RESOURCE_STATE_COMMON,
																	 &clear_value,
																	 IID_PPV_ARGS(g_depth_stencil_buffer.ReleaseAndGetAddressOf()));
			ThrowIfFailed(result, "ID3D12Resource creation failed: {:08X}.", result);
		}

		// Initialize the DSV.
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE dsv_desc_handle = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
			g_device->CreateDepthStencilView(g_depth_stencil_buffer.Get(), nullptr, dsv_desc_handle);
		}

		// Transition the depth stencil buffer.
		{
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource   = g_depth_stencil_buffer.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;

			g_command_list->ResourceBarrier(1u, &barrier);

			ThrowIfFailed(g_command_list->Close());

			ID3D12CommandList* const command_lists[] = { g_command_list.Get() };
			g_command_queue->ExecuteCommandLists(1u, command_lists);

			FlushCommandQueue();
		}

		// Initialize the viewport.
		{
			g_viewport.TopLeftX = 0.0f;
			g_viewport.TopLeftY = 0.0f;
			g_viewport.Width    = static_cast< F32 >(g_display_resolution[0u]);
			g_viewport.Height   = static_cast< F32 >(g_display_resolution[1u]);
			g_viewport.MinDepth = 0.0f;
			g_viewport.MaxDepth = 1.0f;
		}

		// Initialize the scissor rectangle.
		{
			g_scissor_rectangle.left   = 0u;
			g_scissor_rectangle.top    = 0u;
			g_scissor_rectangle.right  = g_display_resolution[0u];
			g_scissor_rectangle.bottom = g_display_resolution[1u];
		}
	}

	void Uninit() noexcept {
		// Switch to windowed mode since Direct3D is incapable of when in 
		// fullscreen mode due to certain threading issues that occur behind
		// the scenes.
		if (g_swap_chain) {
			g_swap_chain->SetFullscreenState(FALSE, nullptr);
		}

		if (g_device) {
			FlushCommandQueue();
		}
	}
	
	void Render() {
		static constexpr F32x4 s_background_color 
			= { 0.0f, 0.117647058f, 0.149019608f, 1.0f };

		ThrowIfFailed(g_command_allocator->Reset());
		ThrowIfFailed(g_command_list->Reset(g_command_allocator.Get(), nullptr));

		// Transition the back buffer.
		{
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource   = g_back_buffers[g_back_buffer_index].Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;

			g_command_list->ResourceBarrier(1u, &barrier);
		}

		D3D12_CPU_DESCRIPTOR_HANDLE rtv_desc_handle = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
		rtv_desc_handle.ptr += g_back_buffer_index * g_rtv_desc_size;
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv_desc_handle = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();

		// OM: Clear the RTV.
		g_command_list->ClearRenderTargetView(rtv_desc_handle, s_background_color.data(), 0u, nullptr);
		// OM: Clear the DSV.
		g_command_list->ClearDepthStencilView(dsv_desc_handle,
											  D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
											  1.0f, 0u, 0u, nullptr);

		// RS: Set the viewport.
		g_command_list->RSSetViewports(1u, &g_viewport);
		// RS: Set the scissor rect.
		g_command_list->RSSetScissorRects(1u, &g_scissor_rectangle);

		// OM: Set the RTV and DSV.
		g_command_list->OMSetRenderTargets(1u, &rtv_desc_handle, true, &dsv_desc_handle);

		// Transition the back buffer.
		{
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource   = g_back_buffers[g_back_buffer_index].Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;

			g_command_list->ResourceBarrier(1u, &barrier);
		}

		ThrowIfFailed(g_command_list->Close());

		ID3D12CommandList* const command_lists[] = { g_command_list.Get() };
		g_command_queue->ExecuteCommandLists(1u, command_lists);

		// Present the back buffer to the front buffer.
		ThrowIfFailed(g_swap_chain->Present(0u, 0u));
		// Swap the current back buffer.
		g_back_buffer_index = (g_back_buffer_index + 1u) % g_nb_back_buffers;

		FlushCommandQueue();
	}

	[[nodiscard]]
	int Run(int nCmdShow) {
		// Show the main window.
		g_window->Show(nCmdShow);

		// Enter the message loop.
		MSG msg;
		SecureZeroMemory(&msg, sizeof(msg));
		while (WM_QUIT != msg.message) {

			// Retrieves messages for any window that belongs to the current
			// thread without performing range filtering. Furthermore messages
			// are removed after processing.
			if (PeekMessage(&msg, nullptr, 0u, 0u, PM_REMOVE)) {
				// Translates virtual-key messages into character messages.
				TranslateMessage(&msg);
				// Dispatches a message to a window procedure.
				DispatchMessage(&msg);
				continue;
			}

			Render();
		}

		Uninit();
		
		return static_cast< int >(msg.wParam);
	}
}

int WINAPI WinMain(_In_ HINSTANCE instance,
				   _In_opt_ [[maybe_unused]] HINSTANCE prev_instance,
				   _In_     [[maybe_unused]] LPSTR lpCmdLine,
				   _In_ int nCmdShow) {

	Init(NotNull< HINSTANCE >(instance));
	return Run(nCmdShow);
}
