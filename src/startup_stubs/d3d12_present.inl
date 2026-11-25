// Experimental (docs/startup-runtime.md): shows the presented surface in the title's window.
// Included inside d3d12_xs_device.cpp's anonymous namespace, before queue_present.
//
// A flip-model swap chain on the title window, created on the queue the title presents with; each
// present draws the presented surface into the back buffer with a full-window triangle (the surface
// is usually larger than the window) and flips. Diagnostics go to the window title, because GDI
// drawing does not mix with a flip-model swap chain.

struct WindowPresenter {
    static constexpr UINT buffer_count = 2;
    struct Frame {
        ID3D12Resource* buffer = nullptr;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> source;
        UINT64 submitted = 0;
    };
    IDXGISwapChain3* chain = nullptr;
    ID3D12DescriptorHeap* targets = nullptr;
    ID3D12DescriptorHeap* textures = nullptr;
    Frame frames[buffer_count];
    ID3D12CommandQueue* queue = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE reached = nullptr;
    HANDLE frame_ready = nullptr;
    UINT64 fence_value = 0;
    UINT64 dxgi_ms = 0, reuse_wait_ms = 0;
    LONG attempts = 0, displayed = 0, reuse_waits = 0, busy_buffers = 0, busy_dxgi = 0;
    UINT width = 0, height = 0;
    bool failed = false;
};
WindowPresenter window_presenter;
LONG window_presents;

// The output window belongs to this layer and has its own thread and message loop; the title's
// window is left alone (a swap chain on it made the title stop at one of its own fatal checks).
HWND output_handle;
DWORD WINAPI output_window_thread(void* ready)
{
    WNDCLASSW cls{};
    cls.lpfnWndProc = [](HWND window, UINT message, WPARAM w, LPARAM l) -> LRESULT {
        if (message == WM_CLOSE) {
            wchar_t name[96]{};
            const DWORD length = GetEnvironmentVariableW(L"X2W_SESSION_CLOSE_EVENT", name, ARRAYSIZE(name));
            if (length && length < ARRAYSIZE(name)) {
                if (HANDLE request = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) {
                    const BOOL sent = SetEvent(request);
                    CloseHandle(request);
                    if (sent) return 0;
                }
            }
            ShowWindow(window, SW_MINIMIZE);
            return 0;
        }
        return DefWindowProcW(window, message, w, l);
    };
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = x2w_startup::presenter_class;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&cls);
    RECT area{0, 0, 1600, 900};
    AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE);
    output_handle = CreateWindowExW(0, cls.lpszClassName, L"Xbox2Windows - output", WS_OVERLAPPEDWINDOW, 60, 60,
                                    area.right - area.left, area.bottom - area.top, nullptr, nullptr, cls.hInstance, nullptr);
    if (output_handle) ShowWindow(output_handle, SW_SHOWNOACTIVATE);
    SetEvent(static_cast<HANDLE>(ready));
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
HWND output_window()
{
    static bool started = false;
    if (!started) {
        started = true;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (HANDLE thread = CreateThread(nullptr, 0, output_window_thread, ready, 0, nullptr)) {
            WaitForSingleObject(ready, 5000);
            CloseHandle(thread);
        }
        CloseHandle(ready);
    }
    return output_handle;
}

bool create_window_presenter(ID3D12CommandQueue* queue, HWND window, bool nonblocking)
{
    WindowPresenter& p = window_presenter;
    ID3D12Device* device = device_of(the_device);
    RECT client{};
    GetClientRect(window, &client);
    p.width = client.right > 0 ? client.right : 1280;
    p.height = client.bottom > 0 ? client.bottom : 720;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    IDXGISwapChain1* chain1 = nullptr;
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = p.width;
        desc.Height = p.height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = p.buffer_count;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (nonblocking) desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &chain1);
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    }
    if (SUCCEEDED(hr)) hr = chain1->QueryInterface(IID_PPV_ARGS(&p.chain));
    if (chain1) chain1->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(hr) && nonblocking) {
        hr = p.chain->SetMaximumFrameLatency(p.buffer_count);
        if (SUCCEEDED(hr)) {
            p.frame_ready = p.chain->GetFrameLatencyWaitableObject();
            if (!p.frame_ready) hr = E_FAIL;
        }
    }
    const D3D12_DESCRIPTOR_HEAP_DESC target_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, p.buffer_count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC texture_heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, p.buffer_count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&target_heap, IID_PPV_ARGS(&p.targets));
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&texture_heap, IID_PPV_ARGS(&p.textures));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; SUCCEEDED(hr) && i < p.buffer_count; ++i) {
        auto& frame = p.frames[i];
        hr = p.chain->GetBuffer(i, IID_PPV_ARGS(&frame.buffer));
        if (SUCCEEDED(hr))
            device->CreateRenderTargetView(frame.buffer, nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{p.targets->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{i} * step});
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator, nullptr, IID_PPV_ARGS(&frame.list));
        if (SUCCEEDED(hr)) hr = frame.list->Close();
    }
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence));
    p.reached = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr) && !p.reached) hr = HRESULT_FROM_WIN32(GetLastError());
    static const char source[] =
        "Texture2D<float4> surface : register(t0); SamplerState smooth : register(s0);\n"
        "struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(V i) : SV_Target0 { return float4(surface.Sample(smooth, i.uv).rgb, 1); }\n";
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* blob = nullptr;
    if (SUCCEEDED(hr)) hr = D3DCompile(source, sizeof(source) - 1, "present", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, nullptr);
    if (SUCCEEDED(hr)) hr = D3DCompile(source, sizeof(source) - 1, "present", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, nullptr);
    if (SUCCEEDED(hr)) {
        const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC desc{1, &parameter, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr);
        if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&p.root));
    }
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = p.root;
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = ~0u;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&p.pipeline));
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (blob) blob->Release();
    log("x2w-present window swap chain %ux%u -> 0x%08lx", p.width, p.height, static_cast<unsigned long>(hr));
    check_device("window presenter", 0);
    if (SUCCEEDED(hr)) p.queue = queue;
    return SUCCEEDED(hr);
}

// Draws `surface` (resting in COMMON) into the window and flips. Returns false if not shown.
bool present_to_window(ID3D12CommandQueue* queue, ID3D12Resource* surface)
{
    static std::mutex mutex;
    std::lock_guard<std::mutex> guard(mutex);
    WindowPresenter& p = window_presenter;
    if (p.failed || !queue || !surface) return false;
    const D3D12_RESOURCE_DESC desc = surface->GetDesc();
    if (!x2w_startup::present_surface_supported(desc)) {
        static LONG refused;
        if (first_calls(refused, 4))
            log("x2w-present surface dim=%d samples=%u flags=0x%x format=%d not supported (present_view_format.h)",
                desc.Dimension, desc.SampleDesc.Count, desc.Flags, desc.Format);
        return false;
    }
    HWND window = output_window();
    if (!window) return false;
    static const bool nonblocking = x2w_local::env_on("X2W_PRESENT_NONBLOCKING");
    if (!p.chain && !create_window_presenter(queue, window, nonblocking)) {
        p.failed = true;
        return false;
    }
    if (queue != p.queue) return false;
    ++p.attempts;
    const auto report_pacing = [&] {
        if (p.attempts != 120) return;
        log("x2w-present pacing attempts=120 displayed=%ld dxgi_ms=%llu reuse_wait_ms=%llu reuse_waits=%ld busy_buffers=%ld busy_dxgi=%ld",
            p.displayed, p.dxgi_ms, p.reuse_wait_ms, p.reuse_waits, p.busy_buffers, p.busy_dxgi);
        p.attempts = p.displayed = 0;
        p.dxgi_ms = p.reuse_wait_ms = 0;
        p.reuse_waits = p.busy_buffers = p.busy_dxgi = 0;
    };
    const UINT index = p.chain->GetCurrentBackBufferIndex();
    auto& frame = p.frames[index];
    auto* list = frame.list;
    const UINT64 completed = p.fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        p.failed = true;
        check_device("window presenter fence", 0);
        return false;
    }
    if (completed < frame.submitted) {
        if (nonblocking) {
            ++p.busy_buffers;
            report_pacing();
            return true;
        }
        const ULONGLONG start = GetTickCount64();
        ++p.reuse_waits;
        // The same event may still be registered after a timeout. Confirm the fence
        // value after waking before reusing the descriptor, allocator or held surface.
        const HRESULT armed = p.fence->SetEventOnCompletion(frame.submitted, p.reached);
        if (SUCCEEDED(armed)) WaitForSingleObject(p.reached, 2000);
        p.reuse_wait_ms += GetTickCount64() - start;
        const UINT64 reached = p.fence->GetCompletedValue();
        if (reached == UINT64_MAX) {
            p.failed = true;
            check_device("window presenter fence", 0);
            return false;
        }
        if (reached < frame.submitted) {
            static LONG stalls;
            if (first_calls(stalls, 5)) log("x2w-present waiting on prior back-buffer work; retaining last image");
            report_pacing();
            return true;
        }
    }
    if (nonblocking && WaitForSingleObject(p.frame_ready, 0) != WAIT_OBJECT_0) {
        ++p.busy_dxgi;
        report_pacing();
        return true;
    }
    ID3D12Device* device = device_of(the_device);
    const UINT texture_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE texture_cpu{p.textures->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{index} * texture_step};
    const D3D12_GPU_DESCRIPTOR_HANDLE texture_gpu{p.textures->GetGPUDescriptorHandleForHeapStart().ptr + UINT64{index} * texture_step};
    const DXGI_FORMAT view_format = x2w_startup::present_view_format(desc.Format);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = view_format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(surface, &view, texture_cpu);
    const D3D12_CPU_DESCRIPTOR_HANDLE target{p.targets->GetCPUDescriptorHandleForHeapStart().ptr +
                                             SIZE_T{index} * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    if (FAILED(frame.allocator->Reset()) || FAILED(list->Reset(frame.allocator, p.pipeline))) {
        report_pacing();
        return false;
    }
    transition(list, surface, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, frame.buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(p.width), static_cast<float>(p.height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(p.width), static_cast<LONG>(p.height)};
    list->SetGraphicsRootSignature(p.root);
    list->SetDescriptorHeaps(1, &p.textures);
    list->SetGraphicsRootDescriptorTable(0, texture_gpu);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, frame.buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    transition(list, surface, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(list->Close())) {
        p.failed = true;
        return false;
    }
    frame.source = surface;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    frame.submitted = ++p.fence_value;
    const HRESULT signal = queue->Signal(p.fence, frame.submitted);
    if (FAILED(signal)) {
        p.failed = true;
        log("x2w-present fence signal failed 0x%08lx; keeping submitted resources alive", static_cast<unsigned long>(signal));
        check_device("window presenter fence", 0);
        return false;
    }
    const ULONGLONG start = GetTickCount64();
    const HRESULT hr = p.chain->Present(0, nonblocking ? DXGI_PRESENT_DO_NOT_WAIT : 0);
    p.dxgi_ms += GetTickCount64() - start;
    if (nonblocking && hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++p.busy_dxgi;
        report_pacing();
        return true;
    }
    static LONG failures;
    if (FAILED(hr) && first_calls(failures, 4)) log("x2w-present Present failed 0x%08lx", static_cast<unsigned long>(hr));
    check_device("window present", 0);
    if (SUCCEEDED(hr)) {
        ++p.displayed;
        InterlockedIncrement(&window_presents);
    }
    report_pacing();
    return SUCCEEDED(hr);
}
