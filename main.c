#include <stdio.h>
#include <stdlib.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <windows.h>
#include "linmath.h"

struct {
	HWND hwnd;
	UINT width;
	UINT height;
	BOOL running;

	UINT framecount;
	UINT frameindex;

	IDXGIFactory4 * factory;
	IDXGISwapChain3 * swapchain;
	ID3D12Device * device;
	ID3D12CommandQueue * cmdqueue;
	ID3D12CommandAllocator * cmdallocator;
	ID3D12GraphicsCommandList * cmdlist;
	ID3D12DescriptorHeap * rtvheap;
	ID3D12Resource * cbo;
	void * cbvdata;
	ID3D12RootSignature * root_sig;
	ID3D12PipelineState * pso;
	ID3D12Resource * vbo;
	D3D12_VERTEX_BUFFER_VIEW vbo_view;
	ID3D12Fence * fence;
	UINT64 fence_value;
	HANDLE fence_event;

	ID3D12Resource * framebuffers[2];

	UINT rtvsize;

	mat4x4 mvp;

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
	.cmdallocator = NULL,
	.cmdlist = NULL,
	.rtvheap = NULL,
	.cbvdata = NULL,
	.cbo = NULL,
	.root_sig = NULL,
	.pso = NULL,
	.vbo = NULL,
	.vbo_view = {
		.BufferLocation = 0,
		.SizeInBytes = 0,
		.StrideInBytes = 0,
	},
	.fence = NULL,
	.fence_value = 0,
	.fence_event = NULL,
	
	.framebuffers = { NULL, NULL },
	
	.rtvsize = 0,

	.mvp = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1,
	},
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

static int push_inited(void ** com) {
	IUnknown *** ptrs = realloc(state.inited.ptrs, sizeof(IUnknown **) * (state.inited.count + 1));
	if (ptrs == NULL) {
		return 1;
	}

	state.inited.ptrs = ptrs;
	state.inited.ptrs[state.inited.count++] = (IUnknown **) com;
	return 0;
}

#define PUSH_INITED(com) { if (push_inited(com) != 0) { BAIL(-1, "Push inited failure\n"); } }
#define BAIL(retval, msg, ...) { fprintf(stderr, msg, __VA_ARGS__); cleanup(); return retval; }
#define BAIL_NO_MSG(retval) { cleanup(); return retval; }

static int wait_for_fence(void) {
	UINT64 fence = state.fence_value;
	if (FAILED(state.cmdqueue->lpVtbl->Signal(state.cmdqueue, state.fence, ++fence))) {
		BAIL(20, "Failed to signal fence\n");
	}
	++state.fence_value;

	if (state.fence->lpVtbl->GetCompletedValue(state.fence) < fence) {
		if (FAILED(state.fence->lpVtbl->SetEventOnCompletion(state.fence, fence, state.fence_event))) {
			BAIL(21, "Failed to set fence event\n");
		}
		WaitForSingleObject(state.fence_event, INFINITE);
	}

	return 0;
}

typedef struct vertex {
	float pos[4];
	float color[4];
} vertex_t;

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

		IDXGISwapChain1 * swapchain;
		if (FAILED(state.factory->lpVtbl->CreateSwapChainForHwnd(state.factory, (IUnknown *) state.cmdqueue, state.hwnd, &swap_desc, NULL, NULL, &swapchain))) {
			BAIL(5, "Failed to create swapchain\n");
		}

		if (FAILED(state.factory->lpVtbl->MakeWindowAssociation(state.factory, state.hwnd, DXGI_MWA_NO_ALT_ENTER))) {
			BAIL(6, "Failed to make window association\n");
		}

		state.swapchain = swapchain;
		PUSH_INITED(&swapchain);
		state.frameindex = state.swapchain->lpVtbl->GetCurrentBackBufferIndex(state.swapchain);
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

		D3D12_CPU_DESCRIPTOR_HANDLE handle;
		state.rtvheap->lpVtbl->GetCPUDescriptorHandleForHeapStart(state.rtvheap, &handle);
		for (UINT i = 0; i < state.framecount; ++i) {
			ID3D12Resource * resource;
			if (FAILED(state.swapchain->lpVtbl->GetBuffer(state.swapchain, i, &IID_ID3D12Resource, &resource))) {
				BAIL(8, "Failed to get swapchain buffer\n");
			}

			state.device->lpVtbl->CreateRenderTargetView(state.device, resource, NULL, handle);
			state.framebuffers[i] = resource;
			PUSH_INITED(&state.framebuffers[i]);

			handle.ptr += state.rtvsize;
		}

		if (FAILED(state.device->lpVtbl->CreateCommandAllocator(state.device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, &state.cmdallocator))) {
			BAIL(9, "Failed to create command allocator\n");
		}
		PUSH_INITED(&state.cmdallocator);
	}

	{
		D3D12_FEATURE_DATA_ROOT_SIGNATURE feat_data = {
			.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1,
		};

		if (FAILED(state.device->lpVtbl->CheckFeatureSupport(state.device, D3D12_FEATURE_ROOT_SIGNATURE, &feat_data, sizeof(feat_data))) {
			feat_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
		}

		D3D12_DESCRIPTOR_RANGE1 range = {
			.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
			.NumDescriptors = 1,
			.BaseShaderRegister = 0,
			.RegisterSpace = 0,
			.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
			.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
		};

		D3D12_ROOT_PARAMETER1 parameters = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
		};

		D3D12_ROOT_SIGNATURE_DESC sig_desc = {
			.NumParameters = 0,
			.pParameters = NULL,
			.NumStaticSamplers = 0,
			.pStaticSamplers = NULL,
			.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
		};

		ID3DBlob * sig;
		ID3DBlob * err;

		if (FAILED(D3D12SerializeRootSignature(&sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
			BAIL(10, "Failed to serialize root signature\n");
		}

		if (FAILED(state.device->lpVtbl->CreateRootSignature(state.device, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, &state.root_sig))) {
			BAIL(11, "Failed to create root signature\n");
		}
		PUSH_INITED(&state.root_sig);

		sig->lpVtbl->Release(sig);
		if (err != NULL) {
			err->lpVtbl->Release(err);
		}
	}

	{
		UINT flags = 0;
		#ifdef _DEBUG
		flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
		#endif

		ID3DBlob * vs;
		ID3DBlob * ps;
		ID3DBlob * err;

		char * src = NULL;
		SIZE_T len = 0;

		{
			FILE * fp = fopen("main.hlsl", "rb");
			if (fp == NULL) {
				BAIL(12, "Failed to open main.hlsl\n");
			}

			fseek(fp, 0, SEEK_END);
			len = ftell(fp);
			fseek(fp, 0, SEEK_SET);

			src = malloc(len + 1);
			if (src == NULL) {
				BAIL(13, "Failed to allocate memory for main.hlsl\n");
			}

			if (fread(src, 1, len, fp) != len) {
				BAIL(14, "Failed to read main.hlsl\n");
			}

			fclose(fp);
		}

		if (FAILED(D3DCompile(src, len, "main.hlsl", NULL, NULL, "vs", "vs_5_0", flags, 0, &vs, &err))) {
			if (err != NULL) {
				OutputDebugStringA(err->lpVtbl->GetBufferPointer(err));
				fprintf(stderr, "D3DCompiler error: %s\n", (char *) err->lpVtbl->GetBufferPointer(err));
				err->lpVtbl->Release(err);
				BAIL(15, "Failed to compile vertex shader\n");
			} else {
				BAIL(15, "Failed to compile vertex shader\n");
			}
		}

		if (FAILED(D3DCompile(src, len, "main.hlsl", NULL, NULL, "ps", "ps_5_0", flags, 0, &ps, &err))) {
			vs->lpVtbl->Release(vs);
			if (err != NULL) {
				OutputDebugStringA(err->lpVtbl->GetBufferPointer(err));
				fprintf(stderr, "D3DCompiler error: %s\n", (char *) err->lpVtbl->GetBufferPointer(err));
				err->lpVtbl->Release(err);
				BAIL(15, "Failed to compile pixel shader\n");
			} else {
				BAIL(15, "Failed to compile pixel shader\n");
			}
		}

		free(src);

		D3D12_INPUT_ELEMENT_DESC input_desc[] = {
			{
				.SemanticName = "POSITION",
				.SemanticIndex = 0,
				.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
				.InputSlot = 0,
				.AlignedByteOffset = 0,
				.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				.InstanceDataStepRate = 0,
			},
			{
				.SemanticName = "COLOR",
				.SemanticIndex = 0,
				.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
				.InputSlot = 0,
				.AlignedByteOffset = 12,
					.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
					.InstanceDataStepRate = 0,
			},
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC ps_desc = {
			.InputLayout = {
				.pInputElementDescs = input_desc,
				.NumElements = sizeof(input_desc) / sizeof(D3D12_INPUT_ELEMENT_DESC),
			},
			.pRootSignature = state.root_sig,
			.VS = {
				vs->lpVtbl->GetBufferPointer(vs),
				vs->lpVtbl->GetBufferSize(vs),
			},
			.PS = {
				ps->lpVtbl->GetBufferPointer(ps),
				ps->lpVtbl->GetBufferSize(ps),
			},
			.RasterizerState = {
				.FillMode = D3D12_FILL_MODE_SOLID,
				.CullMode = D3D12_CULL_MODE_BACK,
				.FrontCounterClockwise = FALSE,
				.DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
				.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
				.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
				.DepthClipEnable = TRUE,
				.MultisampleEnable = FALSE,
				.AntialiasedLineEnable = FALSE,
				.ForcedSampleCount = 0,
				.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
			},
			.BlendState = {
				.AlphaToCoverageEnable = FALSE,
				.IndependentBlendEnable = FALSE,
				.RenderTarget = {
					[0] = {
						.BlendEnable = FALSE,
						.LogicOpEnable = FALSE,
						.SrcBlend = D3D12_BLEND_ONE,
						.DestBlend = D3D12_BLEND_ZERO,
						.BlendOp = D3D12_BLEND_OP_ADD,
						.SrcBlendAlpha = D3D12_BLEND_ONE,
						.DestBlendAlpha = D3D12_BLEND_ZERO,
						.BlendOpAlpha = D3D12_BLEND_OP_ADD,
						.LogicOp = D3D12_LOGIC_OP_NOOP,
						.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
					},
				},
			},
			.DepthStencilState = {
				.DepthEnable = FALSE,
				.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
				.DepthFunc = D3D12_COMPARISON_FUNC_LESS,
				.StencilEnable = FALSE,
				.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK,
				.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK,
				.FrontFace = {
					.StencilFailOp = D3D12_STENCIL_OP_KEEP,
					.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
					.StencilPassOp = D3D12_STENCIL_OP_KEEP,
					.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
				},
			},
			.SampleMask = UINT_MAX,
			.SampleDesc = {
				.Count = 1,
				.Quality = 0,
			},
			.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
			.NumRenderTargets = 1,
			.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM,
			.DSVFormat = DXGI_FORMAT_UNKNOWN,
		};

		if (FAILED(state.device->lpVtbl->CreateGraphicsPipelineState(state.device, &ps_desc, &IID_ID3D12PipelineState, &state.pso))) {
			vs->lpVtbl->Release(vs);
			ps->lpVtbl->Release(ps);
			if (err != NULL) {
				err->lpVtbl->Release(err);
			}
			BAIL(16, "Failed to create pipeline state\n");
		}
		PUSH_INITED(&state.pso);

		vs->lpVtbl->Release(vs);
		ps->lpVtbl->Release(ps);
		if (err != NULL) {
			err->lpVtbl->Release(err);
		}

		if (FAILED(state.device->lpVtbl->CreateCommandList(state.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, state.cmdallocator, NULL, &IID_ID3D12GraphicsCommandList, &state.cmdlist))) {
			BAIL(17, "Failed to create command list\n");
		}
		PUSH_INITED(&state.cmdlist);

		state.cmdlist->lpVtbl->Close(state.cmdlist);
	}

	{
		D3D12_HEAP_PROPERTIES props = {
			.Type = D3D12_HEAP_TYPE_UPLOAD,
			.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
			.CreationNodeMask = 1,
			.VisibleNodeMask = 1,
		};

		D3D12_RESOURCE_DESC desc = {
			.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
			.Alignment = 0,
			.Width = 256,
			.Height = 1,
			.DepthOrArraySize = 1,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_UNKNOWN,
			.SampleDesc = {
				.Count = 1,
				.Quality = 0,
			},
			.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
			.Flags = D3D12_RESOURCE_FLAG_NONE,
		};

		if (FAILED(state.device->lpVtbl->CreateCommittedResource(state.device, &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &state.cbo))) {
			BAIL(18, "Failed to create constant buffer\n");
		}
		PUSH_INITED(&state.cbo);

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {
			.BufferLocation = state.cbo->lpVtbl->GetGPUVirtualAddress(state.cbo),
			.SizeInBytes = 256,
		};

		D3D12_CPU_DESCRIPTOR_HANDLE handle;
		state.rtvheap->lpVtbl->GetCPUDescriptorHandleForHeapStart(state.rtvheap, &handle);
		state.device->lpVtbl->CreateConstantBufferView(state.device, &cbv_desc, handle);

		void * cbegin;
		D3D12_RANGE range = {
			.Begin = 0,
			.End = 0,
		};

		if (FAILED(state.cbo->lpVtbl->Map(state.cbo, 0, &range, &cbegin))) {
			BAIL(19, "Failed to map constant buffer\n");
		}

		state.cbvdata = cbegin;
		memcpy(state.cbvdata, state.mvp, sizeof(state.mvp));
	}

	{
		vertex_t vertices[3] = {
			{  0,  1,  0,  0,		1, 0, 1, 1 },
			{  1, -1,  0,  0,		1, 0, 1, 1 },
			{  2, -1,  0,  0,		1, 0, 1, 1 },
		};

		D3D12_HEAP_PROPERTIES props = {
			.Type = D3D12_HEAP_TYPE_UPLOAD,
			.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
			.CreationNodeMask = 1,
			.VisibleNodeMask = 1,
		};

		D3D12_RESOURCE_DESC desc = {
			.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
			.Alignment = 0,
			.Width = sizeof(vertices),
			.Height = 1,
			.DepthOrArraySize = 1,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_UNKNOWN,
			.SampleDesc = {
				.Count = 1,
				.Quality = 0,
			},
			.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
			.Flags = D3D12_RESOURCE_FLAG_NONE,
		};

		if (FAILED(state.device->lpVtbl->CreateCommittedResource(state.device, &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &state.vbo))) {
			BAIL(18, "Failed to create vertex buffer\n");
		}
		PUSH_INITED(&state.vbo);

		void * vbegin;
		D3D12_RANGE range = {
			.Begin = 0,
			.End = 0,
		};

		if (FAILED(state.vbo->lpVtbl->Map(state.vbo, 0, &range, &vbegin))) {
			BAIL(19, "Failed to map vertex buffer\n");
		}

		memcpy(vbegin, vertices, sizeof(vertices));
		state.vbo->lpVtbl->Unmap(state.vbo, 0, NULL);

		state.vbo_view.BufferLocation = state.vbo->lpVtbl->GetGPUVirtualAddress(state.vbo);
		state.vbo_view.SizeInBytes = sizeof(vertices);
		state.vbo_view.StrideInBytes = sizeof(vertex_t);

		if (FAILED(state.device->lpVtbl->CreateFence(state.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, &state.fence))) {
			BAIL(20, "Failed to create fence\n");
		}
		state.fence_value = 1;

		state.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (state.fence_event == NULL) {
			if (FAILED(HRESULT_FROM_WIN32(GetLastError()))) {
				BAIL(21, "Failed to create fence event\n");
			}
		}

		int err = wait_for_fence();
		if (err != 0) {
			BAIL(err, "Failed to wait for fence\n");
		}
	}

	while (state.running) {
		{
			state.frameindex = state.swapchain->lpVtbl->GetCurrentBackBufferIndex(state.swapchain);
			D3D12_VIEWPORT viewport = {
				.TopLeftX = 0,
				.TopLeftY = 0,
				.Width = state.width,
				.Height = state.height,
				.MinDepth = 0,
				.MaxDepth = 1,
			};

			D3D12_RECT scissor = {
				.left = 0,
				.top = 0,
				.right = state.width,
				.bottom = state.height,
			};

			state.cmdallocator->lpVtbl->Reset(state.cmdallocator);
			state.cmdlist->lpVtbl->Reset(state.cmdlist, state.cmdallocator, state.pso);

			state.cmdlist->lpVtbl->SetGraphicsRootSignature(state.cmdlist, state.root_sig);
			state.cmdlist->lpVtbl->RSSetViewports(state.cmdlist, 1, &viewport);
			state.cmdlist->lpVtbl->RSSetScissorRects(state.cmdlist, 1, &scissor);

			state.cmdlist->lpVtbl->ResourceBarrier(state.cmdlist, 1, (D3D12_RESOURCE_BARRIER[]) {
				{
					.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
						.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
						.Transition = {
							.pResource = state.framebuffers[state.frameindex],
							.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
							.StateBefore = D3D12_RESOURCE_STATE_PRESENT,
							.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET,
					},
				},
			});

			D3D12_CPU_DESCRIPTOR_HANDLE handle;
			state.rtvheap->lpVtbl->GetCPUDescriptorHandleForHeapStart(state.rtvheap, &handle);
			handle.ptr += state.frameindex * state.rtvsize;
			state.cmdlist->lpVtbl->OMSetRenderTargets(state.cmdlist, 1, &handle, FALSE, NULL);

			state.cmdlist->lpVtbl->ClearRenderTargetView(state.cmdlist, handle, (float[4]) { 0, 1, 0, 1 }, 0, NULL);
			state.cmdlist->lpVtbl->IASetPrimitiveTopology(state.cmdlist, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			state.cmdlist->lpVtbl->IASetVertexBuffers(state.cmdlist, 0, 1, &state.vbo_view);
			state.cmdlist->lpVtbl->DrawInstanced(state.cmdlist, 3, 1, 0, 0);

			state.cmdlist->lpVtbl->ResourceBarrier(state.cmdlist, 1, (D3D12_RESOURCE_BARRIER[]) {
				{
					.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
						.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
						.Transition = {
							.pResource = state.framebuffers[state.frameindex],
							.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
							.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
							.StateAfter = D3D12_RESOURCE_STATE_PRESENT,
					},
				},
			});

			if (FAILED(state.cmdlist->lpVtbl->Close(state.cmdlist))) {
				BAIL(22, "Failed to close command list\n");
			}

			int err = wait_for_fence();
			if (err != 0) {
				BAIL(err, "Failed to wait for fence\n");
			}

			state.cmdqueue->lpVtbl->ExecuteCommandLists(state.cmdqueue, 1, (ID3D12CommandList * []) { state.cmdlist });
			state.swapchain->lpVtbl->Present(state.swapchain, 1, 0);

			err = wait_for_fence();
			if (err != 0) {
				BAIL(err, "Failed to wait for fence\n");
			}
		}

		MSG msg;
		if (PeekMessageA(&msg, state.hwnd, 0, 0, PM_REMOVE) != 0) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}

	wait_for_fence();
	CloseHandle(state.fence_event);

	BAIL_NO_MSG(0);
}