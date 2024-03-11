module;

#include <DirectXMath.h>

#include "directx/d3dx12.h"

#include "Shaders/DenoisedComposition.dxil.h"

export module PostProcessing.DenoisedComposition;

import Camera;
import ErrorHelpers;
import GPUBuffer;
import NRD;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct DenoisedComposition {
		struct {
			XMUINT2 RenderSize;
			NRDDenoiser NRDDenoiser;
		} Constants{};

		struct { ConstantBuffer<Camera>* InCamera; } GPUBuffers{};

		struct { D3D12_GPU_DESCRIPTOR_HANDLE InLinearDepth, InBaseColorMetalness, InEmissiveColor, InNormalRoughness, InDenoisedDiffuse, InDenoisedSpecular, OutColor; } GPUDescriptors{};

		explicit DenoisedComposition(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DenoisedComposition_dxil, size(g_DenoisedComposition_dxil) };
			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		void Process(ID3D12GraphicsCommandList* pCommandList) {
			pCommandList->SetPipelineState(m_pipelineStateObject.Get());
			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &Constants, 0);
			pCommandList->SetComputeRootConstantBufferView(1, GPUBuffers.InCamera->GetResource()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootDescriptorTable(2, GPUDescriptors.InLinearDepth);
			pCommandList->SetComputeRootDescriptorTable(3, GPUDescriptors.InBaseColorMetalness);
			pCommandList->SetComputeRootDescriptorTable(4, GPUDescriptors.InEmissiveColor);
			pCommandList->SetComputeRootDescriptorTable(5, GPUDescriptors.InNormalRoughness);
			pCommandList->SetComputeRootDescriptorTable(6, GPUDescriptors.InDenoisedDiffuse);
			pCommandList->SetComputeRootDescriptorTable(7, GPUDescriptors.InDenoisedSpecular);
			pCommandList->SetComputeRootDescriptorTable(8, GPUDescriptors.OutColor);
			pCommandList->Dispatch((Constants.RenderSize.x + 15) / 16, (Constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
	};
}
