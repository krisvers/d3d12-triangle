#include <stdio.h>
#include <stdlib.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <windows.h>

struct {
	HWND hwnd;
	UINT width;
	UINT height;
	BOOL running;

	UINT framecount;
	UINT frameindex;

	IDXGIFactory4 * factory;
	IDXGISwapChain1 * swapchain;
	ID3D12Device * device;
	ID3D12CommandQueue * cmdqueue;
	ID3D12DescriptorHeap * rtvheap;

	UINT rtvsize;

	struct {
		IUnknown *** ptrs;
		UINT count;
	} inited;
} static state = {
	.width = 800,
	.height = 600,
	.running = TRUE,

	.framecount = 2,
	.frameindex = 0,

	.hwnd = NULL,
	.factory = NULL,
	.swapchain = NULL,
	.device = NULL,
	.cmdqueue = NULL,
	.rtvheap = NULL,
	
	.rtvsize = 0,
};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
		case WM_CLOSE: {
			state.running = FALSE;
			return 0;
		}
		case WM_SIZE: {
			state.width = LOWORD(lparam);
			state.height = HIWORD(lparam);
			return 0;
		}
		case WM_DESTROY: {
			PostQuitMessage(0);
			return 0;
		}
	}

	return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static void cleanup(void) {
	if (state.hwnd != NULL) {
		DestroyWindow(state.hwnd);
	}

	if (state.inited.ptrs != NULL) {
		for (UINT i = 0; i < state.inited.count; ++i) {
			if (state.inited.ptrs[i] == NULL) {
				continue;
			}
			if (*(state.inited.ptrs[i]) == NULL) {
				continue;
			}

			(*(state.inited.ptrs[i]))->lpVtbl->Release((*(state.inited.ptrs[i])));
			(*(state.inited.ptrs[i])) = NULL;
			state.inited.ptrs[i] = NULL;
		}

		free(state.inited.ptrs);
	}
}

static int push_inited(IUnknown ** com) {
	IUnknown *** ptrs = realloc(state.inited.ptrs, sizeof(IUnknown **) * (state.inited.count + 1));
	if (ptrs == NULL) {
		return 1;
	}

	state.inited.ptrs = ptrs;
	state.inited.ptrs[state.inited.count++] = com;
	return 0;
}

#define PUSH_INITED(com) { if (push_inited(com) != 0) { BAIL(-1, "Push inited failure\n"); } }
#define BAIL(retval, msg) { fprintf(stderr, msg); cleanup(); return retval; }
#define BAIL_NO_MSG(retval) { cleanup(); return retval; }
int main(void) {
	WNDCLASSEXA wc = {
		.cbSize = sizeof(WNDCLASSEXA),
		.style = CS_HREDRAW | CS_VREDRAW,
		.lpfnWndProc = wnd_proc,
		.cbClsExtra = 0,
		.cbWndExtra = 0,
		.hInstance = GetModuleHandle(NULL),
		.hIcon = LoadIcon(NULL, IDC_ARROW),
		.hCursor = LoadCursor(NULL, IDC_ARROW),
		.hbrBackground = (HBRUSH) GetStockObject(0),
		.lpszClassName = "d3d12",
		.hIconSm = LoadImageA(GetModuleHandle(NULL), MAKEINTRESOURCEA(5), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR),
	};

	RegisterClassExA(&wc);
	
	{
		RECT rect = {
			.top = 0,
			.left = 0,
			.right = state.width,
			.bottom = state.height,
		};
		AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

		state.hwnd = CreateWindowExA(0, wc.lpszClassName, "d3d12", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);
	}

	ShowWindow(state.hwnd, SW_SHOWDEFAULT);
	UpdateWindow(state.hwnd);

	UINT factory_flags = 0;

	#ifdef _DEBUG
	{
		ID3D12Debug * debug;
		if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **) &debug))) {
			debug->lpVtbl->EnableDebugLayer(debug);
			factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}

	{
		IDXGIDebug1 * debug;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, &IID_IDXGIDebug1, (void **) &debug))) {
			debug->lpVtbl->EnableLeakTrackingForThread(debug);
		}
	}
	#endif

	if (FAILED(CreateDXGIFactory2(factory_flags, &IID_IDXGIFactory4, (void **) &state.factory))) {
		BAIL(1, "Failed to create factory\n");
	}
	PUSH_INITED(&state.factory);

	{
		IDXGIAdapter1 * adapter;
		if (FAILED(state.factory->lpVtbl->EnumAdapters1(state.factory, 0, &adapter))) {
			BAIL(2, "Failed to enumerate adapters\n");
		}

		DXGI_ADAPTER_DESC1 desc;
		adapter->lpVtbl->GetDesc1(adapter, &desc);

		if (FAILED(D3D12CreateDevice((IUnknown *) adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, &state.device))) {
			BAIL(3, "Failed to create device\n");
		}
		PUSH_INITED(&state.device);
		adapter->lpVtbl->Release(adapter);
	}

	{
		D3D12_COMMAND_QUEUE_DESC queue_desc = {
			.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
			.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
		};

		if (FAILED(state.device->lpVtbl->CreateCommandQueue(state.device, &queue_desc, &IID_ID3D12CommandQueue, &state.cmdqueue))) {
			BAIL(4, "Failed to create command queue\n");
		}
		PUSH_INITED(&state.cmdqueue);

		DXGI_SWAP_CHAIN_DESC1 swap_desc = {
			.BufferCount = state.framecount,
			.Width = state.width,
			.Height = state.height,
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
			.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
			.SampleDesc = {
				.Count = 1,
				.Quality = 0,
			},
		};

		if (FAILED(state.factory->lpVtbl->CreateSwapChainForHwnd(state.factory, (IUnknown *) state.cmdqueue, state.hwnd, &swap_desc, NULL, NULL, &state.swapchain))) {
			BAIL(5, "Failed to create swapchain\n");
		}
		PUSH_INITED(&state.swapchain);

		if (FAILED(state.factory->lpVtbl->MakeWindowAssociation(state.factory, state.hwnd, DXGI_MWA_NO_ALT_ENTER))) {
			BAIL(6, "Failed to make window association\n");
		}
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
			.NumDescriptors = state.framecount,
			.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
			.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
		};

		if (FAILED(state.device->lpVtbl->CreateDescriptorHeap(state.device, &heap_desc, &IID_ID3D12DescriptorHeap, &state.rtvheap))) {
			BAIL(7, "Failed to create descriptor heap\n");
		}
		PUSH_INITED(&state.rtvheap);

		state.rtvsize = state.device->lpVtbl->GetDescriptorHandleIncrementSize(state.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	while (state.running) {
		MSG msg;
		if (PeekMessageA(&msg, state.hwnd, 0, 0, PM_REMOVE) != 0) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}

	BAIL_NO_MSG(0);
}