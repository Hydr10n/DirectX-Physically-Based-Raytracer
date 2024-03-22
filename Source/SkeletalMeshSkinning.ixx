module;

#include <DirectXMath.h>

#include "directx/d3dx12.h"

#include "Shaders/SkeletalMeshSkinning.dxil.h"

export module SkeletalMeshSkinning;

import ErrorHelpers;
import GPUBuffer;
import Model;
import Vertex;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export struct SkeletalMeshSkinning {
	struct {
		DefaultBuffer<VertexPositionNormalTangentBones>* InSkeletalVertices;
		UploadBuffer<XMFLOAT3X4>* InSkeletalTransforms;
		DefaultBuffer<Mesh::VertexType>* OutVertices;
		DefaultBuffer<XMFLOAT3>* OutMotionVectors;
	} GPUBuffers{};

	explicit SkeletalMeshSkinning(ID3D12Device* pDevice) noexcept(false) {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_SkeletalMeshSkinning_dxil, size(g_SkeletalMeshSkinning_dxil) };
		ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
		m_pipelineState->SetName(L"SkeletalMeshSkinning");
	}

	void Prepare(ID3D12GraphicsCommandList* pCommandList) {
		pCommandList->SetPipelineState(m_pipelineState.Get());
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
	}

	void Process(ID3D12GraphicsCommandList* pCommandList) {
		const auto vertexCount = static_cast<UINT>(GPUBuffers.OutVertices->GetCount());
		pCommandList->SetComputeRoot32BitConstant(0, vertexCount, 0);
		pCommandList->SetComputeRootShaderResourceView(1, GPUBuffers.InSkeletalVertices->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(2, GPUBuffers.InSkeletalTransforms->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootUnorderedAccessView(3, GPUBuffers.OutVertices->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootUnorderedAccessView(4, GPUBuffers.OutMotionVectors->GetNative()->GetGPUVirtualAddress());
		pCommandList->Dispatch((vertexCount + 255) / 256, 1, 1);
		const auto barriers = { CD3DX12_RESOURCE_BARRIER::UAV(*GPUBuffers.OutVertices), CD3DX12_RESOURCE_BARRIER::UAV(*GPUBuffers.OutMotionVectors) };
		return pCommandList->ResourceBarrier(static_cast<UINT>(size(barriers)), data(barriers));
	}

private:
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;
};
