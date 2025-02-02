module;

#include <DirectXMath.h>

#include "directx/d3dx12.h"

#include "Shaders/SkeletalMeshSkinning.dxil.h"

export module SkeletalMeshSkinning;

import CommandList;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export struct SkeletalMeshSkinning {
	struct { GPUBuffer* SkeletalVertices, * SkeletalTransforms, * Vertices, * MotionVectors; } GPUBuffers{};

	SkeletalMeshSkinning(const SkeletalMeshSkinning&) = delete;
	SkeletalMeshSkinning& operator=(const SkeletalMeshSkinning&) = delete;

	explicit SkeletalMeshSkinning(const DeviceContext& deviceContext) noexcept(false) {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_SkeletalMeshSkinning_dxil, size(g_SkeletalMeshSkinning_dxil) };

		ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

		const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
		m_pipelineState->SetName(L"SkeletalMeshSkinning");
	}

	void Prepare(CommandList& commandList) {
		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetPipelineState(m_pipelineState.Get());
	}

	void Process(CommandList& commandList) {
		const auto vertexCount = static_cast<UINT>(GPUBuffers.Vertices->GetCapacity());

		commandList.SetState(*GPUBuffers.SkeletalVertices, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*GPUBuffers.SkeletalTransforms, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*GPUBuffers.Vertices, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*GPUBuffers.MotionVectors, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		commandList->SetComputeRoot32BitConstant(0, vertexCount, 0);
		commandList->SetComputeRootShaderResourceView(1, GPUBuffers.SkeletalVertices->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(2, GPUBuffers.SkeletalTransforms->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootUnorderedAccessView(3, GPUBuffers.Vertices->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootUnorderedAccessView(4, GPUBuffers.MotionVectors->GetNative()->GetGPUVirtualAddress());

		commandList->Dispatch((vertexCount + 255) / 256, 1, 1);

		commandList.SetState(*GPUBuffers.Vertices, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*GPUBuffers.MotionVectors, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	}

private:
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;
};
