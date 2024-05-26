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
		DefaultBuffer<VertexPositionNormalTangentBones>* SkeletalVertices;
		UploadBuffer<XMFLOAT3X4>* SkeletalTransforms;
		DefaultBuffer<Mesh::VertexType>* Vertices;
		DefaultBuffer<XMFLOAT3>* MotionVectors;
	} GPUBuffers{};

	explicit SkeletalMeshSkinning(ID3D12Device* pDevice) noexcept(false) {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_SkeletalMeshSkinning_dxil, size(g_SkeletalMeshSkinning_dxil) };

		ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

		const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
		m_pipelineState->SetName(L"SkeletalMeshSkinning");
	}

	void Prepare(ID3D12GraphicsCommandList* pCommandList) {
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		pCommandList->SetPipelineState(m_pipelineState.Get());
	}

	void Process(ID3D12GraphicsCommandList* pCommandList) {
		GPUBuffers.Vertices->InsertUAVBarrier(pCommandList);
		GPUBuffers.MotionVectors->InsertUAVBarrier(pCommandList);

		const auto vertexCount = static_cast<UINT>(GPUBuffers.Vertices->GetCount());

		pCommandList->SetComputeRoot32BitConstant(0, vertexCount, 0);
		pCommandList->SetComputeRootShaderResourceView(1, GPUBuffers.SkeletalVertices->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(2, GPUBuffers.SkeletalTransforms->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootUnorderedAccessView(3, GPUBuffers.Vertices->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootUnorderedAccessView(4, GPUBuffers.MotionVectors->GetNative()->GetGPUVirtualAddress());

		pCommandList->Dispatch((vertexCount + 255) / 256, 1, 1);
	}

private:
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;
};
