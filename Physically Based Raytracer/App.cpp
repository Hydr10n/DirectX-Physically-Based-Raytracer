module;

#include "pch.h"

#include "DeviceResources.h"

#include "StepTimer.h"

#include "RenderTexture.h"

#include "directxtk12/GraphicsMemory.h"

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/SimpleMath.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ImGuiEx.h"

#include "MyAppData.h"

#include <shellapi.h>

#include "Shaders/Raytracing.hlsl.h"

module App;

import Camera;
import DirectX.BufferHelpers;
import DirectX.DescriptorHeapEx;
import DirectX.RaytracingHelpers;
import HaltonSamplePattern;
import Model;
import MyScene;
import ShaderCommonData;
import SharedData;

using namespace DirectX;
using namespace DirectX::BufferHelpers;
using namespace DirectX::RaytracingHelpers;
using namespace DirectX::SimpleMath;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;
using namespace std::filesystem;
using namespace WindowHelpers;

using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;
using Key = Keyboard::Keys;

namespace {
	constexpr auto& GraphicsSettings = MyAppData::Settings::Graphics;
	constexpr auto& UISettings = MyAppData::Settings::UI;
	constexpr auto& ControlsSettings = MyAppData::Settings::Controls;
}

#define MAKE_OBJECT_NAME(Name) static constexpr LPCSTR Name = #Name;

struct App::Impl : IDeviceNotify {
	Impl(const shared_ptr<WindowModeHelper>& windowModeHelper) noexcept(false) : m_windowModeHelper(windowModeHelper) {
		{
			{
				{
					auto& cameraSettings = GraphicsSettings.Camera;
					cameraSettings.VerticalFieldOfView = clamp(cameraSettings.VerticalFieldOfView, CameraMinVerticalFieldOfView, CameraMaxVerticalFieldOfView);
				}

				{
					auto& raytracingSettings = GraphicsSettings.Raytracing;
					raytracingSettings.MaxTraceRecursionDepth = clamp(raytracingSettings.MaxTraceRecursionDepth, 1u, RaytracingMaxTraceRecursionDepth);
					raytracingSettings.SamplesPerPixel = clamp(raytracingSettings.SamplesPerPixel, 1u, RaytracingMaxSamplesPerPixel);
				}

				{
					auto& postProcessingSettings = GraphicsSettings.PostProcessing;

					{
						auto& toneMappingSettings = postProcessingSettings.ToneMapping;
						toneMappingSettings.Operator = clamp(toneMappingSettings.Operator, ToneMapPostProcess::None, static_cast<ToneMapPostProcess::Operator>(ToneMapPostProcess::Operator_Max - 1));
						toneMappingSettings.Exposure = clamp(toneMappingSettings.Exposure, 0.0f, ToneMappingMaxExposure);
					}

					{
						auto& bloomSettings = postProcessingSettings.Bloom;
						bloomSettings.Threshold = clamp(bloomSettings.Threshold, 0.0f, 1.0f);
						bloomSettings.BlurSize = clamp(bloomSettings.BlurSize, 1.0f, BloomMaxBlurSize);
					}
				}
			}

			{
				auto& menuSettings = UISettings.Menu;
				menuSettings.BackgroundOpacity = clamp(menuSettings.BackgroundOpacity, 0.0f, 1.0f);
			}

			{
				auto& speedSettings = ControlsSettings.Camera.Speed;
				speedSettings.Movement = clamp(speedSettings.Movement, CameraMinMovementSpeed, CameraMaxMovementSpeed);
				speedSettings.Rotation = clamp(speedSettings.Rotation, CameraMinRotationSpeed, CameraMaxRotationSpeed);
			}
		}

		{
			m_firstPersonCamera.SetPosition(m_scene.Camera.Position);
			m_firstPersonCamera.SetDirections(m_scene.Camera.Directions.Up, m_scene.Camera.Directions.Forward);
		}

		windowModeHelper->SetFullscreenResolutionHandledByWindow(false);

		{
			ImGui::CreateContext();

			ImGui::StyleColorsDark();

			auto& IO = ImGui::GetIO();

			IO.IniFilename = IO.LogFilename = nullptr;

			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
			IO.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			ImGui_ImplWin32_Init(m_windowModeHelper->hWnd);
		}

		{
			m_deviceResources->RegisterDeviceNotify(this);

			m_deviceResources->SetWindow(windowModeHelper->hWnd, windowModeHelper->GetResolution());

			m_deviceResources->EnableVSync(GraphicsSettings.IsVSyncEnabled);

			m_deviceResources->CreateDeviceResources();
			CreateDeviceDependentResources();

			m_deviceResources->CreateWindowSizeDependentResources();
			CreateWindowSizeDependentResources();
		}

		m_inputDevices.Mouse->SetWindow(windowModeHelper->hWnd);
	}

	~Impl() {
		m_deviceResources->WaitForGpu();

		{
			if (ImGui::GetIO().BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();

			ImGui_ImplWin32_Shutdown();

			ImGui::DestroyContext();
		}
	}

	SIZE GetOutputSize() const noexcept { return m_deviceResources->GetOutputSize(); }

	float GetOutputAspectRatio() const noexcept {
		const auto [cx, cy] = GetOutputSize();
		return static_cast<float>(cx) / static_cast<float>(cy);
	}

	void Tick() {
		m_stepTimer.Tick([&] { Update(); });

		Render();

		m_isViewChanged = false;

		if (m_isWindowSettingChanged) {
			ThrowIfFailed(m_windowModeHelper->Apply());
			m_isWindowSettingChanged = false;
		}
	}

	void OnWindowSizeChanged() { if (m_deviceResources->WindowSizeChanged(m_windowModeHelper->GetResolution())) CreateWindowSizeDependentResources(); }

	void OnDisplayChanged() { m_deviceResources->UpdateColorSpace(); }

	void OnResuming() {
		m_stepTimer.ResetElapsedTime();

		m_inputDevices.Gamepad->Resume();

		m_inputDeviceStateTrackers = {};
	}

	void OnSuspending() { m_inputDevices.Gamepad->Suspend(); }

	void OnActivated() {
		m_inputDeviceStateTrackers.Keyboard.Reset();
		m_inputDeviceStateTrackers.Mouse.Reset();
	}

	void OnDeactivated() {}

	void OnDeviceLost() override {
		ImGui_ImplDX12_Shutdown();

		m_renderTextures = {};

		for (auto& textures : m_environmentCubeMaps | views::values) for (auto& [_, texture] : get<0>(textures)) texture.Resource.Reset();

		for (auto& model : m_scene.Models | views::values) model->Meshes.clear();

		m_shaderBuffers = {};

		m_topLevelAccelerationStructure.reset();
		m_bottomLevelAccelerationStructures = {};

		m_bloom = {};

		m_toneMapping = {};

		m_pipelineState.Reset();

		m_rootSignature.Reset();

		m_resourceDescriptorHeap.reset();

		m_graphicsMemory.reset();
	}

	void OnDeviceRestored() override {
		CreateDeviceDependentResources();

		CreateWindowSizeDependentResources();
	}

private:
	const shared_ptr<WindowModeHelper> m_windowModeHelper;

	unique_ptr<DeviceResources> m_deviceResources = make_unique<DeviceResources>(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_D32_FLOAT, 2, D3D_FEATURE_LEVEL_12_1, D3D12_RAYTRACING_TIER_1_1, DeviceResources::c_AllowTearing);

	StepTimer m_stepTimer;

	unique_ptr<GraphicsMemory> m_graphicsMemory;

	const struct {
		unique_ptr<GamePad> Gamepad = make_unique<DirectX::GamePad>();
		unique_ptr<Keyboard> Keyboard = make_unique<DirectX::Keyboard>();
		unique_ptr<Mouse> Mouse = make_unique<DirectX::Mouse>();
	} m_inputDevices;

	struct {
		GamePad::ButtonStateTracker Gamepad;
		Keyboard::KeyboardStateTracker Keyboard;
		Mouse::ButtonStateTracker Mouse;
	} m_inputDeviceStateTrackers;

	struct ObjectNames {
		MAKE_OBJECT_NAME(EnvironmentLight);
		MAKE_OBJECT_NAME(Environment);
	};

	struct ResourceDescriptorHeapIndex {
		enum {
			LocalResourceDescriptorHeapIndices,
			Camera,
			GlobalData, LocalData,
			OutputSRV, OutputUAV,
			FinalOutput,
			Blur1, Blur2,
			EnvironmentLightCubeMap, EnvironmentCubeMap,
			Font,
			Reserve,
			Count = 0x400
		};
	};
	unique_ptr<DescriptorHeapEx> m_resourceDescriptorHeap;

	struct RenderDescriptorHeapIndex {
		enum {
			FinalOutput,
			Blur1, Blur2,
			Count
		};
	};
	unique_ptr<DescriptorHeapEx> m_renderDescriptorHeap;

	ComPtr<ID3D12RootSignature> m_rootSignature;

	ComPtr<ID3D12PipelineState> m_pipelineState;

	static constexpr float ToneMappingMaxExposure = 5;
	map<ToneMapPostProcess::Operator, shared_ptr<ToneMapPostProcess>> m_toneMapping;

	static constexpr float BloomMaxBlurSize = 4;
	struct {
		unique_ptr<BasicPostProcess> Extraction, Blur;
		unique_ptr<DualPostProcess> Combination;
	} m_bloom;

	map<shared_ptr<ModelMesh>, shared_ptr<BottomLevelAccelerationStructure>> m_bottomLevelAccelerationStructures;
	unique_ptr<TopLevelAccelerationStructure> m_topLevelAccelerationStructure;

	struct {
		unique_ptr<ConstantBuffer<GlobalResourceDescriptorHeapIndices>> GlobalResourceDescriptorHeapIndices;
		unique_ptr<ConstantBuffer<Camera>> Camera;
		unique_ptr<ConstantBuffer<GlobalData>> GlobalData;
		unique_ptr<StructuredBuffer<LocalResourceDescriptorHeapIndices>> LocalResourceDescriptorHeapIndices;
		unique_ptr<StructuredBuffer<LocalData>> LocalData;
	} m_shaderBuffers;

	struct { unique_ptr<RenderTexture> Output, FinalOutput, Blur1, Blur2; } m_renderTextures;

	static constexpr float
		CameraMinVerticalFieldOfView = 30, CameraMaxVerticalFieldOfView = 120,
		CameraMinMovementSpeed = 0.1f, CameraMaxMovementSpeed = 100, CameraMinRotationSpeed = 0.01f, CameraMaxRotationSpeed = 5;
	bool m_isViewChanged = true;
	FirstPersonCamera m_firstPersonCamera;

	HaltonSamplePattern m_haltonSamplePattern;

	MyScene m_scene;

	TextureDictionary m_environmentCubeMaps;

	static constexpr UINT RaytracingMaxTraceRecursionDepth = 32, RaytracingMaxSamplesPerPixel = 16;

	bool m_isMenuOpen = UISettings.Menu.IsOpenOnStartup;

	bool m_isWindowSettingChanged{};

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_graphicsMemory = make_unique<GraphicsMemory>(device);

		CreateDescriptorHeaps();

		CreateRootSignatures();

		CreatePipelineStates();

		CreatePostProcess();

		LoadScene();

		CreateAccelerationStructures();

		CreateShaderBuffers();
	}

	void CreateWindowSizeDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto outputSize = GetOutputSize();

		m_isViewChanged = true;

		{
			m_firstPersonCamera.SetLens(XMConvertToRadians(GraphicsSettings.Camera.VerticalFieldOfView), GetOutputAspectRatio());
			m_shaderBuffers.Camera->GetData().ProjectionToWorld = XMMatrixTranspose(m_firstPersonCamera.InverseViewProjection());
		}

		{
			const auto CreateResource = [&](DXGI_FORMAT format, unique_ptr<RenderTexture>& texture, const SIZE& size, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u, UINT rtvDescriptorHeapIndex = ~0u) {
				texture = make_unique<RenderTexture>(format);
				texture->SetDevice(device, m_resourceDescriptorHeap.get(), srvDescriptorHeapIndex, uavDescriptorHeapIndex, m_renderDescriptorHeap.get(), rtvDescriptorHeapIndex);
				texture->CreateResource(size.cx, size.cy);
			};

			CreateResource(DXGI_FORMAT_R32G32B32A32_FLOAT, m_renderTextures.Output, outputSize, ResourceDescriptorHeapIndex::OutputSRV, ResourceDescriptorHeapIndex::OutputUAV);

			const auto backBufferFormat = m_deviceResources->GetBackBufferFormat();

			CreateResource(backBufferFormat, m_renderTextures.FinalOutput, outputSize, ResourceDescriptorHeapIndex::FinalOutput, ~0u, RenderDescriptorHeapIndex::FinalOutput);

			{
				const SIZE size{ outputSize.cx / 2, outputSize.cy / 2 };
				CreateResource(backBufferFormat, m_renderTextures.Blur1, size, ResourceDescriptorHeapIndex::Blur1, ~0u, RenderDescriptorHeapIndex::Blur1);
				CreateResource(backBufferFormat, m_renderTextures.Blur2, size, ResourceDescriptorHeapIndex::Blur2, ~0u, RenderDescriptorHeapIndex::Blur2);
			}
		}

		for (const auto& toneMapping : m_toneMapping | views::values) toneMapping->SetHDRSourceTexture(m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::OutputSRV));

		{
			auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();
			ImGui_ImplDX12_Init(device, static_cast<int>(m_deviceResources->GetBackBufferCount()), m_deviceResources->GetBackBufferFormat(), m_resourceDescriptorHeap->Heap(), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::Font), m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::Font));

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\segoeui.ttf)", static_cast<float>(outputSize.cy) * 0.025f);
		}
	}

	void Update() {
		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Update");

		ProcessInput();

		m_shaderBuffers.Camera->GetData().Jitter = m_haltonSamplePattern.GetNext();

		UpdateGlobalData();

		PIXEndEvent();
	}

	void Render() {
		if (!m_stepTimer.GetFrameCount()) return;

		m_deviceResources->Prepare();

		const auto commandList = m_deviceResources->GetCommandList();

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Clear");

		const auto renderTargetView = m_deviceResources->GetRenderTargetView(), depthStencilView = m_deviceResources->GetDepthStencilView();
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, &depthStencilView);
		commandList->ClearRenderTargetView(renderTargetView, Colors::Black, 0, nullptr);
		commandList->ClearDepthStencilView(depthStencilView, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0, 0, nullptr);

		const auto viewport = m_deviceResources->GetScreenViewport();
		const auto scissorRect = m_deviceResources->GetScissorRect();
		commandList->RSSetViewports(1, &viewport);
		commandList->RSSetScissorRects(1, &scissorRect);

		PIXEndEvent(commandList);

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

		const auto descriptorHeaps = { m_resourceDescriptorHeap->Heap() };
		commandList->SetDescriptorHeaps(static_cast<UINT>(size(descriptorHeaps)), descriptorHeaps.begin());

		DispatchRays();

		PostProcess();

		if (m_isMenuOpen) RenderMenu();

		PIXEndEvent(commandList);

		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Present");

		m_deviceResources->Present();

		m_deviceResources->WaitForGpu();

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		PIXEndEvent();
	}

	void LoadScene() {
		const auto device = m_deviceResources->GetD3DDevice();
		const auto commandQueue = m_deviceResources->GetCommandQueue();

		{
			UINT descriptorHeapIndex = ResourceDescriptorHeapIndex::Reserve;
			m_scene.Models.Load(device, commandQueue, *m_resourceDescriptorHeap, descriptorHeapIndex, 8);
		}

		{
			decltype(m_environmentCubeMaps) environmentCubeMaps;

			if (!m_scene.EnvironmentLightCubeMap.FilePath.empty()) {
				environmentCubeMaps[ObjectNames::EnvironmentLight] = {
					{
						{
							TextureType::CubeMap,
							Texture{
								.DescriptorHeapIndices{
									.SRV = ResourceDescriptorHeapIndex::EnvironmentLightCubeMap
								},
								.FilePath = m_scene.EnvironmentLightCubeMap.FilePath
							}
						}
					},
					m_scene.EnvironmentLightCubeMap.Transform
				};
			}

			if (!m_scene.EnvironmentCubeMap.FilePath.empty()) {
				environmentCubeMaps[ObjectNames::Environment] = {
					{
						{
							TextureType::CubeMap,
							Texture{
								.DescriptorHeapIndices{
									.SRV = ResourceDescriptorHeapIndex::EnvironmentCubeMap
								},
								.FilePath = m_scene.EnvironmentCubeMap.FilePath
							}
						}
					},
					m_scene.EnvironmentCubeMap.Transform
				};
			}

			if (!environmentCubeMaps.empty()) {
				environmentCubeMaps.DirectoryPath = m_scene.EnvironmentCubeMapDirectoryPath;
				environmentCubeMaps.Load(device, commandQueue, *m_resourceDescriptorHeap, 2);

				m_environmentCubeMaps = move(environmentCubeMaps);
			}
		}
	}

	void CreateDescriptorHeaps() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_resourceDescriptorHeap = make_unique<DescriptorHeapEx>(device, ResourceDescriptorHeapIndex::Count, ResourceDescriptorHeapIndex::Reserve);

		m_renderDescriptorHeap = make_unique<DescriptorHeapEx>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, RenderDescriptorHeapIndex::Count);
	}

	void CreateRootSignatures() {
		const auto device = m_deviceResources->GetD3DDevice();

		ThrowIfFailed(device->CreateRootSignature(0, g_pRaytracing, size(g_pRaytracing), IID_PPV_ARGS(&m_rootSignature)));
	}

	void CreatePipelineStates() {
		const auto device = m_deviceResources->GetD3DDevice();

		const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pRaytracing, size(g_pRaytracing));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
		ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
	}

	void CreatePostProcess() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto backBufferFormat = m_deviceResources->GetBackBufferFormat();

		for (const auto toneMappingOperator : { ToneMapPostProcess::None, ToneMapPostProcess::Saturate, ToneMapPostProcess::Reinhard, ToneMapPostProcess::ACESFilmic }) {
			auto& toneMapping = m_toneMapping[toneMappingOperator];
			toneMapping = make_shared<ToneMapPostProcess>(device, RenderTargetState(backBufferFormat, DXGI_FORMAT_UNKNOWN), toneMappingOperator, toneMappingOperator == ToneMapPostProcess::None ? ToneMapPostProcess::Linear : ToneMapPostProcess::SRGB);
			toneMapping->SetExposure(GraphicsSettings.PostProcessing.ToneMapping.Exposure);
		}

		{
			const RenderTargetState renderTargetState(backBufferFormat, m_deviceResources->GetDepthBufferFormat());
			m_bloom = {
				.Extraction = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomExtract),
				.Blur = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomBlur),
				.Combination = make_unique<DualPostProcess>(device, renderTargetState, DualPostProcess::BloomCombine),
			};
		}
	}

	void CreateTopLevelAccelerationStructure(bool updateOnly) {
		vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
		instanceDescs.reserve(size(m_scene.RenderItems));
		for (UINT i = 0; const auto & renderItem : m_scene.RenderItems) {
			for (const auto& model = *renderItem.pModel->get(); const auto & mesh : model.Meshes) {
				D3D12_RAYTRACING_INSTANCE_DESC instanceDesc;
				auto transform = mesh->Transform * renderItem.Transform;
				if (model.LoaderFlags & Model::LoaderFlags::RightHanded) transform *= XMMatrixScaling(1, 1, -1);
				XMStoreFloat3x4(reinterpret_cast<XMFLOAT3X4*>(instanceDesc.Transform), transform);
				instanceDesc.InstanceID = i++;
				instanceDesc.InstanceMask = ~0u;
				instanceDesc.InstanceContributionToHitGroupIndex = 0;
				if (renderItem.IsVisible) {
					instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
					if (model.LoaderFlags & Model::LoaderFlags::RightHanded) instanceDesc.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
				}
				else instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
				instanceDesc.AccelerationStructure = m_bottomLevelAccelerationStructures.at(mesh)->GetBuffer()->GetGPUVirtualAddress();
				instanceDescs.emplace_back(instanceDesc);
			}
		}
		m_topLevelAccelerationStructure->Build(m_deviceResources->GetCommandList(), instanceDescs, updateOnly);
	}

	void CreateAccelerationStructures() {
		const auto device = m_deviceResources->GetD3DDevice();
		const auto commandList = m_deviceResources->GetCommandList();

		ThrowIfFailed(commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr));

		for (const auto& model : m_scene.Models | views::values) {
			for (const auto& mesh : model->Meshes) {
				if (const auto ret = m_bottomLevelAccelerationStructures.try_emplace(mesh); ret.second) {
					auto& bottomLevelAccelerationStructure = ret.first->second;
					bottomLevelAccelerationStructure = make_shared<BottomLevelAccelerationStructure>(device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);
					bottomLevelAccelerationStructure->Build(m_deviceResources->GetCommandList(), initializer_list{ mesh->GetGeometryDesc() }, false);
				}
			}
		}

		m_topLevelAccelerationStructure = make_unique<TopLevelAccelerationStructure>(device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
		CreateTopLevelAccelerationStructure(false);

		ThrowIfFailed(commandList->Close());

		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(1, CommandListCast(&commandList));

		m_deviceResources->WaitForGpu();
	}

	void CreateShaderBuffers() {
		const auto device = m_deviceResources->GetD3DDevice();

		{
			const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, const auto & data, UINT descriptorHeapIndex = ~0u) {
				uploadBuffer = make_unique<T>(device);
				uploadBuffer->GetData() = data;
				if (descriptorHeapIndex != ~0u) uploadBuffer->CreateConstantBufferView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			XMMATRIX environmentLightCubeMapTransform, environmentCubeMapTransform;

			{
				const auto GetEnvironmentCubeMap = [&](LPCSTR name, UINT& srvDescriptorHeapIndex, XMMATRIX& transform) {
					if (const auto pTextures = m_environmentCubeMaps.find(name); pTextures != m_environmentCubeMaps.cend()) {
						const auto& textures = get<0>(pTextures->second);
						if (const auto pTexture = textures.find(TextureType::CubeMap); pTexture != textures.cend()) {
							srvDescriptorHeapIndex = pTexture->second.DescriptorHeapIndices.SRV;
							transform = get<1>(pTextures->second);
							return;
						}
					}
					srvDescriptorHeapIndex = ~0u;
					transform = XMMatrixIdentity();
				};

				UINT environmentLightCubeMapDescriptorHeapIndex, environmentCubeMapDescriptorHeapIndex;
				GetEnvironmentCubeMap(ObjectNames::EnvironmentLight, environmentLightCubeMapDescriptorHeapIndex, environmentLightCubeMapTransform);
				GetEnvironmentCubeMap(ObjectNames::Environment, environmentCubeMapDescriptorHeapIndex, environmentCubeMapTransform);

				CreateBuffer(
					m_shaderBuffers.GlobalResourceDescriptorHeapIndices,
					GlobalResourceDescriptorHeapIndices{
						.LocalResourceDescriptorHeapIndices = ResourceDescriptorHeapIndex::LocalResourceDescriptorHeapIndices,
						.Camera = ResourceDescriptorHeapIndex::Camera,
						.GlobalData = ResourceDescriptorHeapIndex::GlobalData,
						.LocalData = ResourceDescriptorHeapIndex::LocalData,
						.Output = ResourceDescriptorHeapIndex::OutputUAV,
						.EnvironmentLightCubeMap = environmentLightCubeMapDescriptorHeapIndex,
						.EnvironmentCubeMap = environmentCubeMapDescriptorHeapIndex
					}
				);
			}

			CreateBuffer(m_shaderBuffers.Camera, Camera{ .Position = m_firstPersonCamera.GetPosition() }, ResourceDescriptorHeapIndex::Camera);

			CreateBuffer(
				m_shaderBuffers.GlobalData,
				GlobalData{
					.RaytracingMaxTraceRecursionDepth = GraphicsSettings.Raytracing.MaxTraceRecursionDepth,
					.RaytracingSamplesPerPixel = GraphicsSettings.Raytracing.SamplesPerPixel,
					.EnvironmentLightColor = m_scene.EnvironmentLightColor,
					.EnvironmentLightCubeMapTransform = m_scene.EnvironmentLightCubeMap.Transform,
					.EnvironmentColor = m_scene.EnvironmentColor,
					.EnvironmentCubeMapTransform = m_scene.EnvironmentCubeMap.Transform
				},
				ResourceDescriptorHeapIndex::GlobalData
			);
		}

		{
			const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, UINT count, UINT descriptorHeapIndex) {
				uploadBuffer = make_unique<T>(device, count);
				uploadBuffer->CreateShaderResourceView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			const auto instanceDescCount = m_topLevelAccelerationStructure->GetDescCount();

			CreateBuffer(m_shaderBuffers.LocalResourceDescriptorHeapIndices, instanceDescCount, ResourceDescriptorHeapIndex::LocalResourceDescriptorHeapIndices);

			CreateBuffer(m_shaderBuffers.LocalData, instanceDescCount, ResourceDescriptorHeapIndex::LocalData);

			for (UINT i = 0; const auto & renderItem : m_scene.RenderItems) {
				for (const auto& mesh : renderItem.pModel->get()->Meshes) {
					auto& localResourceDescriptorHeapIndices = (*m_shaderBuffers.LocalResourceDescriptorHeapIndices)[i];

					localResourceDescriptorHeapIndices = {
						.TriangleMesh{
							.Vertices = mesh->DescriptorHeapIndices.Vertices,
							.Indices = mesh->DescriptorHeapIndices.Indices
						}
					};

					auto& localData = (*m_shaderBuffers.LocalData)[i];

					localData.HasPerVertexTangents = mesh->HasPerVertexTangents;

					localData.Material = mesh->Material;

					if (const auto pTextures = mesh->Textures.find(""); pTextures != mesh->Textures.cend()) {
						for (const auto& [textureType, texture] : get<0>(pTextures->second)) {
							auto& textures = localResourceDescriptorHeapIndices.Textures;
							UINT* p;
							switch (textureType) {
							case TextureType::BaseColorMap: p = &textures.BaseColorMap; break;
							case TextureType::EmissiveMap: p = &textures.EmissiveMap; break;
							case TextureType::SpecularMap: p = &textures.SpecularMap; break;
							case TextureType::MetallicMap: p = &textures.MetallicMap; break;
							case TextureType::RoughnessMap: p = &textures.RoughnessMap; break;
							case TextureType::AmbientOcclusionMap: p = &textures.AmbientOcclusionMap; break;
							case TextureType::OpacityMap: p = &textures.OpacityMap; break;
							case TextureType::NormalMap: p = &textures.NormalMap; break;
							default: p = nullptr; break;
							}
							if (p != nullptr) *p = texture.DescriptorHeapIndices.SRV;
						}
						localData.TextureTransform = XMMatrixTranspose(get<1>(pTextures->second));
					}

					i++;
				}
			}
		}
	}

	void ProcessInput() {
		const auto gamepadState = m_inputDevices.Gamepad->GetState(0);
		auto& gamepadStateTracker = m_inputDeviceStateTrackers.Gamepad;
		if (gamepadState.IsConnected()) gamepadStateTracker.Update(gamepadState);
		else gamepadStateTracker.Reset();

		const auto keyboardState = m_inputDevices.Keyboard->GetState();
		auto& keyboardStateTracker = m_inputDeviceStateTrackers.Keyboard;
		keyboardStateTracker.Update(keyboardState);

		const auto mouseState = m_inputDevices.Mouse->GetState();
		auto& mouseStateTracker = m_inputDeviceStateTrackers.Mouse;
		mouseStateTracker.Update(mouseState);

		{
			if (gamepadStateTracker.menu == GamepadButtonState::PRESSED) m_isMenuOpen = !m_isMenuOpen;
			if (keyboardStateTracker.IsKeyPressed(Key::Escape)) m_isMenuOpen = !m_isMenuOpen;
		}

		if (m_isMenuOpen) m_inputDevices.Mouse->SetMode(Mouse::MODE_ABSOLUTE);
		else {
			m_inputDevices.Mouse->SetMode(Mouse::MODE_RELATIVE);

			UpdateCamera(gamepadState, keyboardState, mouseState);
		}
	}

	void UpdateCamera(const GamePad::State& gamepadState, const Keyboard::State& keyboardState, const Mouse::State& mouseState) {
		const auto elapsedSeconds = static_cast<float>(m_stepTimer.GetElapsedSeconds());

		const auto& rightDirection = m_firstPersonCamera.GetRightDirection(), & upDirection = m_firstPersonCamera.GetUpDirection(), & forwardDirection = m_firstPersonCamera.GetForwardDirection();

		const auto Translate = [&](const XMFLOAT3& displacement) {
			if (displacement.x == 0 && displacement.y == 0 && displacement.z == 0) return;

			m_isViewChanged = true;

			m_firstPersonCamera.Translate(rightDirection * displacement.x + upDirection * displacement.y + forwardDirection * displacement.z);
		};

		const auto Yaw = [&](float angle) {
			if (angle == 0) return;

			m_isViewChanged = true;

			m_firstPersonCamera.Yaw(angle);
		};

		const auto Pitch = [&](float angle) {
			if (angle == 0) return;

			m_isViewChanged = true;

			if (const auto pitch = asin(forwardDirection.y);
				pitch - angle > XM_PIDIV2) angle = -max(0.0f, XM_PIDIV2 - pitch - 0.1f);
			else if (pitch - angle < -XM_PIDIV2) angle = -min(0.0f, XM_PIDIV2 + pitch + 0.1f);
			m_firstPersonCamera.Pitch(angle);
		};

		const auto& speed = ControlsSettings.Camera.Speed;

		if (gamepadState.IsConnected()) {
			const auto translationSpeed = elapsedSeconds * 10 * speed.Movement * (gamepadState.IsLeftTriggerPressed() ? 0.5f : 1) * (gamepadState.IsRightTriggerPressed() ? 2.0f : 1);
			const XMFLOAT3 displacement{ gamepadState.thumbSticks.leftX * translationSpeed, 0, gamepadState.thumbSticks.leftY * translationSpeed };
			Translate(displacement);

			const auto rotationSpeed = elapsedSeconds * XM_2PI * 0.4f * speed.Rotation;
			Yaw(gamepadState.thumbSticks.rightX * rotationSpeed);
			Pitch(gamepadState.thumbSticks.rightY * rotationSpeed);
		}

		if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
			const auto translationSpeed = elapsedSeconds * 10 * speed.Movement * (keyboardState.LeftControl ? 0.5f : 1) * (keyboardState.LeftShift ? 2.0f : 1);
			XMFLOAT3 displacement{};
			if (keyboardState.A) displacement.x -= translationSpeed;
			if (keyboardState.D) displacement.x += translationSpeed;
			if (keyboardState.W) displacement.z += translationSpeed;
			if (keyboardState.S) displacement.z -= translationSpeed;
			Translate(displacement);

			const auto rotationSpeed = elapsedSeconds * 20 * speed.Rotation;
			Yaw(XMConvertToRadians(static_cast<float>(mouseState.x)) * rotationSpeed);
			Pitch(XMConvertToRadians(static_cast<float>(mouseState.y)) * rotationSpeed);
		}

		if (m_isViewChanged) {
			auto& camera = m_shaderBuffers.Camera->GetData();
			camera.Position = m_firstPersonCamera.GetPosition();
			camera.ProjectionToWorld = XMMatrixTranspose(m_firstPersonCamera.InverseViewProjection());
		}
	}

	void UpdateGlobalData() {
		auto& globalData = m_shaderBuffers.GlobalData->GetData();

		globalData.FrameCount = m_stepTimer.GetFrameCount();
		globalData.AccumulatedFrameIndex = m_isViewChanged ? 0 : globalData.AccumulatedFrameIndex + 1;
	}

	void DispatchRays() {
		CreateTopLevelAccelerationStructure(true);

		const auto commandList = m_deviceResources->GetCommandList();

		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetComputeRootShaderResourceView(0, m_topLevelAccelerationStructure->GetBuffer()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(1, m_shaderBuffers.GlobalResourceDescriptorHeapIndices->GetResource()->GetGPUVirtualAddress());

		commandList->SetPipelineState(m_pipelineState.Get());

		const auto outputSize = GetOutputSize();
		commandList->Dispatch(static_cast<UINT>((outputSize.cx + 16) / 16), static_cast<UINT>((outputSize.cy + 16) / 16), 1);
	}

	void PostProcess() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto isBloomEnabled = GraphicsSettings.PostProcessing.Bloom.IsEnabled;

		if (isBloomEnabled) {
			const auto finalOutputRTV = m_renderDescriptorHeap->GetCpuHandle(m_renderTextures.FinalOutput->GetRtvDescriptorHeapIndex());
			commandList->OMSetRenderTargets(1, &finalOutputRTV, FALSE, nullptr);
		}

		m_toneMapping.at(GraphicsSettings.PostProcessing.ToneMapping.Operator)->Process(commandList);

		if (isBloomEnabled) PostProcessBloom();
	}

	void PostProcessBloom() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& bloomSettings = GraphicsSettings.PostProcessing.Bloom;

		const auto& [Extraction, Blur, Combination] = m_bloom;

		auto& finalOutput = *m_renderTextures.FinalOutput, & blur1 = *m_renderTextures.Blur1, & blur2 = *m_renderTextures.Blur2;
		const auto finalOutputResource = finalOutput.GetResource(), blur1Resource = blur1.GetResource(), blur2Resource = blur2.GetResource();
		const auto
			finalOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(finalOutput.GetSrvDescriptorHeapIndex()),
			blur1SRV = m_resourceDescriptorHeap->GetGpuHandle(blur1.GetSrvDescriptorHeapIndex()),
			blur2SRV = m_resourceDescriptorHeap->GetGpuHandle(blur2.GetSrvDescriptorHeapIndex());

		const auto
			blur1RTV = m_renderDescriptorHeap->GetCpuHandle(blur1.GetRtvDescriptorHeapIndex()),
			blur2RTV = m_renderDescriptorHeap->GetCpuHandle(blur2.GetRtvDescriptorHeapIndex());

		finalOutput.TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		Extraction->SetSourceTexture(finalOutputSRV, finalOutputResource);
		Extraction->SetBloomExtractParameter(bloomSettings.Threshold);

		commandList->OMSetRenderTargets(1, &blur1RTV, FALSE, nullptr);

		const auto viewPort = m_deviceResources->GetScreenViewport();

		auto halfViewPort = viewPort;
		halfViewPort.Height /= 2;
		halfViewPort.Width /= 2;
		commandList->RSSetViewports(1, &halfViewPort);

		Extraction->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		Blur->SetSourceTexture(blur1SRV, blur1Resource);
		Blur->SetBloomBlurParameters(true, bloomSettings.BlurSize, 1);

		commandList->OMSetRenderTargets(1, &blur2RTV, FALSE, nullptr);

		Blur->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
		blur2.TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		Blur->SetSourceTexture(blur2SRV, blur2Resource);
		Blur->SetBloomBlurParameters(false, bloomSettings.BlurSize, 1);

		commandList->OMSetRenderTargets(1, &blur1RTV, FALSE, nullptr);

		Blur->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		Combination->SetSourceTexture(finalOutputSRV);
		Combination->SetSourceTexture2(blur1SRV);
		Combination->SetBloomCombineParameters(1.25f, 1, 1, 1);

		const auto renderTargetView = m_deviceResources->GetRenderTargetView();
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);

		commandList->RSSetViewports(1, &viewPort);

		Combination->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
		blur2.TransitionTo(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
		finalOutput.TransitionTo(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	void RenderMenu() {
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		{
			const auto outputSize = GetOutputSize();

			ImGui::SetNextWindowPos({});
			ImGui::SetNextWindowSize({ static_cast<float>(outputSize.cx), 0 });
			ImGui::SetNextWindowBgAlpha(UISettings.Menu.BackgroundOpacity);

			ImGui::Begin("Menu", &m_isMenuOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_HorizontalScrollbar);

			if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) ImGui::SetWindowFocus();

			if (ImGui::CollapsingHeader("Settings")) {
				if (ImGui::TreeNode("Graphics")) {
					auto isChanged = false;

					{
						const auto windowModes = { "Windowed", "Borderless", "Fullscreen" };
						if (auto windowMode = m_windowModeHelper->GetMode();
							ImGui::Combo("Window Mode", reinterpret_cast<int*>(&windowMode), windowModes.begin(), static_cast<int>(size(windowModes)))) {
							m_windowModeHelper->SetMode(windowMode);

							GraphicsSettings.WindowMode = windowMode;
							m_isWindowSettingChanged = isChanged = true;
						}
					}

					{
						const auto ToString = [](const SIZE& value) { return format("{} × {}", value.cx, value.cy); };
						if (const auto resolution = m_windowModeHelper->GetResolution();
							ImGui::BeginCombo("Resolution", ToString(resolution).c_str())) {
							for (const auto& displayResolution : g_displayResolutions) {
								const auto isSelected = resolution == displayResolution;
								if (ImGui::Selectable(ToString(displayResolution).c_str(), isSelected)) {
									m_windowModeHelper->SetResolution(displayResolution);

									GraphicsSettings.Resolution = displayResolution;
									isChanged = m_isWindowSettingChanged = true;
								}
								if (isSelected) ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}
					}

					{
						const ImGuiEx::ScopedEnablement scopedEnablement(m_deviceResources->GetDeviceOptions() & DeviceResources::c_AllowTearing);

						if (auto isEnabled = m_deviceResources->IsVSyncEnabled(); ImGui::Checkbox("V-Sync", &isEnabled) && m_deviceResources->EnableVSync(isEnabled)) {
							GraphicsSettings.IsVSyncEnabled = isEnabled;

							isChanged = true;
						}
					}

					if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& cameraSettings = GraphicsSettings.Camera;

						if (ImGui::SliderFloat("Vertical Field of View", &cameraSettings.VerticalFieldOfView, CameraMinVerticalFieldOfView, CameraMaxVerticalFieldOfView, "%.1f", ImGuiSliderFlags_NoInput)) {
							m_firstPersonCamera.SetLens(XMConvertToRadians(cameraSettings.VerticalFieldOfView), GetOutputAspectRatio());
							m_shaderBuffers.Camera->GetData().ProjectionToWorld = XMMatrixTranspose(m_firstPersonCamera.InverseViewProjection());

							m_shaderBuffers.GlobalData->GetData().AccumulatedFrameIndex = 0;

							isChanged = true;
						}

						ImGui::TreePop();
					}

					if (ImGui::TreeNodeEx("Raytracing", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& raytracingSettings = GraphicsSettings.Raytracing;

						auto& globalData = m_shaderBuffers.GlobalData->GetData();

						if (ImGui::SliderInt("Max Trace Recursion Depth", reinterpret_cast<int*>(&raytracingSettings.MaxTraceRecursionDepth), 1, RaytracingMaxTraceRecursionDepth, "%d", ImGuiSliderFlags_NoInput)) {
							globalData.RaytracingMaxTraceRecursionDepth = raytracingSettings.MaxTraceRecursionDepth;
							globalData.AccumulatedFrameIndex = 0;

							isChanged = true;
						}

						if (ImGui::SliderInt("Samples Per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, static_cast<int>(RaytracingMaxSamplesPerPixel), "%d", ImGuiSliderFlags_NoInput)) {
							globalData.RaytracingSamplesPerPixel = raytracingSettings.SamplesPerPixel;
							globalData.AccumulatedFrameIndex = 0;

							isChanged = true;
						}

						ImGui::TreePop();
					}

					if (ImGui::TreeNodeEx("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
						if (ImGui::TreeNodeEx("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& toneMappingSettings = GraphicsSettings.PostProcessing.ToneMapping;

							bool isOperatorChanged;
							{
								const auto operators = { "None", "Saturate", "Reinhard", "ACES Filmic" };
								isChanged |= isOperatorChanged = ImGui::Combo("Operator", reinterpret_cast<int*>(&toneMappingSettings.Operator), operators.begin(), static_cast<int>(size(operators)));
							}

							bool isExposureChanged;
							isChanged |= isExposureChanged = toneMappingSettings.Operator == ToneMapPostProcess::None ? false : ImGui::SliderFloat("Exposure", &toneMappingSettings.Exposure, 0, ToneMappingMaxExposure, "%.2f", ImGuiSliderFlags_NoInput);

							if (isOperatorChanged || isExposureChanged) m_toneMapping[toneMappingSettings.Operator]->SetExposure(toneMappingSettings.Exposure);

							ImGui::TreePop();
						}

						if (ImGui::TreeNodeEx("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& bloomSettings = GraphicsSettings.PostProcessing.Bloom;

							isChanged |= ImGui::Checkbox("Enable", &bloomSettings.IsEnabled);

							if (bloomSettings.IsEnabled) {
								isChanged |= ImGui::SliderFloat("Threshold", &bloomSettings.Threshold, 0, 1, "%.2f", ImGuiSliderFlags_NoInput);

								isChanged |= ImGui::SliderFloat("Blur size", &bloomSettings.BlurSize, 1, BloomMaxBlurSize, "%.2f", ImGuiSliderFlags_NoInput);
							}

							ImGui::TreePop();
						}

						ImGui::TreePop();
					}

					ImGui::TreePop();

					if (isChanged) ignore = GraphicsSettings.Save();
				}

				if (ImGui::TreeNode("UI")) {
					auto isChanged = false;

					if (ImGui::TreeNodeEx("Menu", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& menuSettings = UISettings.Menu;

						isChanged |= ImGui::Checkbox("Open on Startup", &menuSettings.IsOpenOnStartup);

						isChanged |= ImGui::SliderFloat("Background Opacity", &menuSettings.BackgroundOpacity, 0, 1, "%.2f", ImGuiSliderFlags_NoInput);

						ImGui::TreePop();
					}

					ImGui::TreePop();

					if (isChanged) ignore = UISettings.Save();
				}

				{
					ImGui::PushID(0);

					if (ImGui::TreeNode("Controls")) {
						auto isChanged = false;

						if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& cameraSettings = ControlsSettings.Camera;

							if (ImGui::TreeNodeEx("Speed", ImGuiTreeNodeFlags_DefaultOpen)) {
								auto& speedSettings = cameraSettings.Speed;

								isChanged |= ImGui::SliderFloat("Movement", &speedSettings.Movement, CameraMinMovementSpeed, CameraMaxMovementSpeed, "%.1f", ImGuiSliderFlags_NoInput);

								isChanged |= ImGui::SliderFloat("Rotation", &speedSettings.Rotation, CameraMinRotationSpeed, CameraMaxRotationSpeed, "%.2f", ImGuiSliderFlags_NoInput);

								ImGui::TreePop();
							}

							ImGui::TreePop();
						}

						ImGui::TreePop();

						if (isChanged) ignore = ControlsSettings.Save();
					}

					ImGui::PopID();
				}
			}

			if (ImGui::CollapsingHeader("Controls")) {
				const auto AddContents = [](LPCSTR treeLabel, LPCSTR tableID, const initializer_list<pair<LPCSTR, LPCSTR>>& list) {
					if (ImGui::TreeNode(treeLabel)) {
						if (ImGui::BeginTable(tableID, 2, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInner)) {
							for (const auto& [first, second] : list) {
								ImGui::TableNextRow();

								ImGui::TableSetColumnIndex(0);
								ImGui::Text(first);

								ImGui::TableSetColumnIndex(1);
								ImGui::Text(second);
							}

							ImGui::EndTable();
						}

						ImGui::TreePop();
					}
				};

				AddContents(
					"Xbox Controller",
					"##XboxController",
					{
						{ "Menu", "Open/close menu" },
						{ "LS (rotate)", "Move" },
						{ "LT (hold)", "Move slower" },
						{ "RT (hold)", "Move faster" },
						{ "RS (rotate)", "Look around" }
					}
				);

				AddContents(
					"Keyboard",
					"##Keyboard",
					{
						{ "Alt + Enter", "Toggle between windowed/borderless and fullscreen modes" },
						{ "Esc", "Open/close menu" },
						{ "W A S D", "Move" },
						{ "Left Ctrl (hold)", "Move slower" },
						{ "Left Shift (hold)", "Move faster" }
					}
				);

				AddContents(
					"Mouse",
					"##Mouse",
					{
						{ "(Move)", "Look around" }
					}
				);
			}

			if (ImGui::CollapsingHeader("About")) {
				ImGui::Text("© Hydr10n. All rights reserved.");

				if (constexpr auto URL = "https://github.com/Hydr10n/DirectX-Physically-Based-Raytracer";
					ImGuiEx::Hyperlink("GitHub repository", URL)) {
					ShellExecuteA(nullptr, "open", URL, nullptr, nullptr, SW_SHOW);
				}
			}

			{
				ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, { 0, 0.5f });
				if (ImGui::Button("Exit", { -FLT_MIN, 0 })) PostQuitMessage(ERROR_SUCCESS);
				ImGui::PopStyleVar();
			}

			ImGui::End();
		}

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_deviceResources->GetCommandList());
	}
};

App::App(const shared_ptr<WindowModeHelper>& windowModeHelper) : m_impl(make_unique<Impl>(windowModeHelper)) {}

App::~App() = default;

SIZE App::GetOutputSize() const noexcept { return m_impl->GetOutputSize(); }

float App::GetOutputAspectRatio() const noexcept { return m_impl->GetOutputAspectRatio(); }

void App::Tick() { m_impl->Tick(); }

void App::OnWindowSizeChanged() { m_impl->OnWindowSizeChanged(); }

void App::OnDisplayChanged() { m_impl->OnDisplayChanged(); }

void App::OnResuming() { m_impl->OnResuming(); }

void App::OnSuspending() { m_impl->OnSuspending(); }

void App::OnActivated() { return m_impl->OnActivated(); }

void App::OnDeactivated() { return m_impl->OnDeactivated(); }
