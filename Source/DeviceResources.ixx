//
// A wrapper for the Direct3D 12 device and swapchain
//

module;

#include "directx/d3dx12.h"
#include <dxgi1_6.h>

#include <wrl.h>

export module DeviceResources;

export namespace DX
{
    class IDeviceNotify
    {
    public:
        virtual void OnDeviceLost() = 0;
        virtual void OnDeviceRestored() = 0;

    protected:
        ~IDeviceNotify() = default;
    };

    class DeviceResources
    {
    public:
        static constexpr unsigned int c_ReverseDepth      = 0x1;
        static constexpr unsigned int c_DisableGpuTimeout = 0x2;

        DeviceResources(DXGI_FORMAT backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM,
                        DXGI_FORMAT depthBufferFormat = DXGI_FORMAT_D32_FLOAT,
                        UINT backBufferCount = 2,
                        D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL_12_0,
                        D3D12_RAYTRACING_TIER minRaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED,
                        unsigned int flags = 0) noexcept(false);
        ~DeviceResources();

        DeviceResources(DeviceResources&&) = default;
        DeviceResources& operator= (DeviceResources&&) = default;

        DeviceResources(DeviceResources const&) = delete;
        DeviceResources& operator= (DeviceResources const&) = delete;

        void CreateDeviceResources();
        void CreateWindowSizeDependentResources();
        void SetWindow(HWND window, SIZE size) noexcept;
        bool EnableVSync(bool value) noexcept;
        void RequestHDR(bool value) noexcept;
        bool ResizeWindow(SIZE size);
        void HandleDeviceLost();
        void RegisterDeviceNotify(IDeviceNotify* deviceNotify) noexcept { m_deviceNotify = deviceNotify; }
        void Prepare(D3D12_RESOURCE_STATES beforeState = D3D12_RESOURCE_STATE_PRESENT,
                     D3D12_RESOURCE_STATES afterState = D3D12_RESOURCE_STATE_RENDER_TARGET);
        void Present(D3D12_RESOURCE_STATES beforeState = D3D12_RESOURCE_STATE_RENDER_TARGET);
        void WaitForGpu() noexcept;
        void UpdateColorSpace();

        // Device Accessors.
        SIZE GetOutputSize() const noexcept { return m_outputSize; }
        
        // Direct3D Accessors.
        auto                        GetDevice() const noexcept               { return m_device.Get(); }
        auto                        GetSwapChain() const noexcept            { return m_swapChain.Get(); }
        auto                        GetAdapter() const noexcept              { return m_adapter.Get(); }
        auto                        GetDXGIFactory() const noexcept          { return m_dxgiFactory.Get(); }
        HWND                        GetWindow() const noexcept               { return m_window; }
        D3D_FEATURE_LEVEL           GetDeviceFeatureLevel() const noexcept   { return m_d3dFeatureLevel; }
        D3D12_RAYTRACING_TIER       GetDeviceRaytracingTier() const noexcept { return m_raytracingTier; }
        ID3D12Resource*             GetRenderTarget() const noexcept         { return m_renderTargets[m_backBufferIndex].Get(); }
        ID3D12Resource*             GetDepthStencil() const noexcept         { return m_depthStencil.Get(); }
        ID3D12CommandQueue*         GetCommandQueue() const noexcept         { return m_commandQueue.Get(); }
        ID3D12CommandAllocator*     GetCommandAllocator() const noexcept     { return m_commandAllocators[m_backBufferIndex].Get(); }
        auto                        GetCommandList() const noexcept          { return m_commandList.Get(); }
        DXGI_FORMAT                 GetBackBufferFormat() const noexcept     { return m_backBufferFormat; }
        DXGI_FORMAT                 GetDepthBufferFormat() const noexcept    { return m_depthBufferFormat; }
        D3D12_VIEWPORT              GetScreenViewport() const noexcept       { return m_screenViewport; }
        D3D12_RECT                  GetScissorRect() const noexcept          { return m_scissorRect; }
        UINT                        GetCurrentFrameIndex() const noexcept    { return m_backBufferIndex; }
        UINT                        GetBackBufferCount() const noexcept      { return m_backBufferCount; }
        DXGI_COLOR_SPACE_TYPE       GetColorSpace() const noexcept           { return m_colorSpace; }
        unsigned int                GetDeviceOptions() const noexcept        { return m_options; }
        bool                        IsTearingSupported() const noexcept      { return m_isTearingSupported; }
        bool                        IsVSyncEnabled() const noexcept          { return m_isVSyncEnabled; }
        bool                        IsHDRSupported() const noexcept          { return m_isHDRSupported; }
        bool                        IsHDREnabled() const noexcept            { return m_isHDRRequested && m_isHDRSupported; }

        CD3DX12_CPU_DESCRIPTOR_HANDLE GetRenderTargetView() const noexcept
        {
            return CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                static_cast<INT>(m_backBufferIndex), m_rtvDescriptorSize);
        }
        CD3DX12_CPU_DESCRIPTOR_HANDLE GetDepthStencilView() const noexcept
        {
            return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
        }

    private:
        static constexpr size_t MAX_BACK_BUFFER_COUNT = 3;

        UINT                                                m_backBufferIndex{};

        // Direct3D objects.
        Microsoft::WRL::ComPtr<ID3D12Device5>               m_device;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4>  m_commandList;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue>          m_commandQueue;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>      m_commandAllocators[MAX_BACK_BUFFER_COUNT];

        // Swap chain objects.
        Microsoft::WRL::ComPtr<IDXGIFactory4>               m_dxgiFactory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1>               m_adapter;
        Microsoft::WRL::ComPtr<IDXGISwapChain3>             m_swapChain;
        Microsoft::WRL::ComPtr<ID3D12Resource>              m_renderTargets[MAX_BACK_BUFFER_COUNT];
        Microsoft::WRL::ComPtr<ID3D12Resource>              m_depthStencil;

        // Presentation fence objects.
        Microsoft::WRL::ComPtr<ID3D12Fence>                 m_fence;
        UINT64                                              m_fenceValues[MAX_BACK_BUFFER_COUNT]{};
        Microsoft::WRL::Wrappers::Event                     m_fenceEvent;

        // Direct3D rendering objects.
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        m_rtvDescriptorHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        m_dsvDescriptorHeap;
        UINT                                                m_rtvDescriptorSize{};
        D3D12_VIEWPORT                                      m_screenViewport{};
        D3D12_RECT                                          m_scissorRect{};

        // Direct3D properties.
        DXGI_FORMAT                                         m_backBufferFormat;
        DXGI_FORMAT                                         m_depthBufferFormat;
        UINT                                                m_backBufferCount;
        D3D_FEATURE_LEVEL                                   m_d3dMinFeatureLevel;
        D3D12_RAYTRACING_TIER                               m_minRaytracingTier;

        // Cached device properties.
        HWND                                                m_window{};
        D3D_FEATURE_LEVEL                                   m_d3dFeatureLevel = D3D_FEATURE_LEVEL_12_0;
        D3D12_RAYTRACING_TIER                               m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        DWORD                                               m_dxgiFactoryFlags{};
        SIZE                                                m_outputSize{};

        // DeviceResources options (see flags above)
        unsigned int                                        m_options;

        bool                                                m_isTearingSupported{};
        bool                                                m_isVSyncEnabled = true;

        // HDR Support
        bool                                                m_isHDRRequested{};
        bool                                                m_isHDRSupported{};
        DXGI_COLOR_SPACE_TYPE                               m_colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

        // The IDeviceNotify can be held directly as it owns the DeviceResources.
        IDeviceNotify*                                      m_deviceNotify{};

        void MoveToNextFrame();
        void CreateDevice();
    };
}
