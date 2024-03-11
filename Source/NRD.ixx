module;

#include <ranges>

#include <d3d12.h>

#include "NRD.h"
#include "NRI.h"
#include "NRIDescs.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "NRDIntegration.hpp"

export module NRD;

using namespace nrd;
using namespace nri;
using namespace std;

export {
	enum class NRDDenoiser { None, ReBLUR, ReLAX };

	struct NRD {
		NRD(const NRD&) = delete;
		NRD& operator=(const NRD&) = delete;

		NRD(
			ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, ID3D12GraphicsCommandList* pCommandList,
			uint16_t resourceWidth, uint16_t resourceHeight,
			uint32_t backBufferCount,
			span<const DenoiserDesc> denoiserDescs
		) : m_NRD(backBufferCount, true) {
			if (nriCreateDeviceFromD3D12Device({ .d3d12Device = pDevice, .d3d12GraphicsQueue = pCommandQueue }, m_device) == nri::Result::SUCCESS
				&& nriGetInterface(*m_device, NRI_INTERFACE(nri::CoreInterface), static_cast<CoreInterface*>(&m_NRI)) == nri::Result::SUCCESS
				&& nriGetInterface(*m_device, NRI_INTERFACE(nri::HelperInterface), static_cast<HelperInterface*>(&m_NRI)) == nri::Result::SUCCESS
				&& nriGetInterface(*m_device, NRI_INTERFACE(nri::WrapperD3D12Interface), static_cast<WrapperD3D12Interface*>(&m_NRI)) == nri::Result::SUCCESS
				&& m_NRI.CreateCommandBufferD3D12(*m_device, CommandBufferD3D12Desc{ .d3d12CommandList = pCommandList }, m_commandBuffer) == nri::Result::SUCCESS) {
				m_isAvailable = m_NRD.Initialize(resourceWidth, resourceHeight, { .denoisers = data(denoiserDescs), .denoisersNum = static_cast<uint32_t>(size(denoiserDescs)) }, *m_device, m_NRI, m_NRI);
				if (m_isAvailable) m_textureBarrierDescs.resize(NrdUserPool().max_size());
			}
		}

		~NRD() {
			if (m_isAvailable) m_NRD.Destroy();

			for (const auto& textureBarrierDesc : m_textureBarrierDescs) {
				if (textureBarrierDesc.texture != nullptr) m_NRI.DestroyTexture(const_cast<Texture&>(*textureBarrierDesc.texture));
			}

			if (m_commandBuffer != nullptr) m_NRI.DestroyCommandBuffer(*m_commandBuffer);

			if (m_device != nullptr) nriDestroyDevice(*m_device);
		}

		auto IsAvailable() const { return m_isAvailable; }

		void NewFrame() { m_NRD.NewFrame(); }

		auto Tag(ResourceType type, ID3D12Resource* pResource, D3D12_RESOURCE_STATES state) {
			if (!m_isAvailable || pResource == nullptr) return false;

			AccessBits accessBits;
			Layout layout;
			if (state & D3D12_RESOURCE_STATE_RENDER_TARGET) {
				accessBits = AccessBits::COLOR_ATTACHMENT;
				layout = Layout::COLOR_ATTACHMENT;
			}
			else if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
				accessBits = AccessBits::SHADER_RESOURCE_STORAGE;
				layout = Layout::UNKNOWN;
			}
			else if (state & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) {
				accessBits = AccessBits::SHADER_RESOURCE;
				layout = Layout::SHADER_RESOURCE;
			}
			else return false;

			auto& textureBarrierDesc = m_textureBarrierDescs[static_cast<size_t>(type)];

			textureBarrierDesc.initial = textureBarrierDesc.after = { accessBits, layout };

			if (textureBarrierDesc.resource == pResource) return true;

			Texture* texture;
			if (m_NRI.CreateTextureD3D12(*m_device, { pResource }, texture) != nri::Result::SUCCESS) return false;
			if (textureBarrierDesc.texture != nullptr) m_NRI.DestroyTexture(const_cast<Texture&>(*textureBarrierDesc.texture));
			textureBarrierDesc.texture = texture;

			NrdIntegration_SetResource(m_userPool, type, { &textureBarrierDesc, nriConvertDXGIFormatToNRI(pResource->GetDesc().Format) });

			textureBarrierDesc.resource = pResource;

			return true;
		}

		auto SetConstants(const CommonSettings& commonSettings) { return m_NRD.SetCommonSettings(commonSettings); }

		template <typename T>
		auto SetConstants(Identifier denoiser, const T& denoiserSettings) { return m_NRD.SetDenoiserSettings(denoiser, &denoiserSettings); }

		void Denoise(span<const Identifier> denoisers) {
			m_NRD.Denoise(data(denoisers), static_cast<uint32_t>(size(denoisers)), *m_commandBuffer, m_userPool);

			vector<TextureBarrierDesc> textureBarrierDescs;
			for (auto& textureTransitionBarrierDesc : m_textureBarrierDescs) {
				if (textureTransitionBarrierDesc.texture != nullptr
					&& (textureTransitionBarrierDesc.after.access != textureTransitionBarrierDesc.initial.access
						|| textureTransitionBarrierDesc.after.layout != textureTransitionBarrierDesc.initial.layout)) {
					textureTransitionBarrierDesc.before = textureTransitionBarrierDesc.after;
					textureTransitionBarrierDesc.after = textureTransitionBarrierDesc.initial;
					textureBarrierDescs.emplace_back(textureTransitionBarrierDesc);
				}
			}
			m_NRI.CmdBarrier(*m_commandBuffer, BarrierGroupDesc{ .textures = data(textureBarrierDescs), .textureNum = static_cast<uint16_t>(size(textureBarrierDescs)) });
		}

		auto GetTotalMemoryUsageInMb() const { return m_NRD.GetTotalMemoryUsageInMb(); }
		auto GetPersistentMemoryUsageInMb() const { return m_NRD.GetPersistentMemoryUsageInMb(); }
		auto GetAliasableMemoryUsageInMb() const { return m_NRD.GetAliasableMemoryUsageInMb(); }

		static constexpr auto ToDXGIFormat(NormalEncoding value) {
			switch (value) {
				case NormalEncoding::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
				case NormalEncoding::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
				case NormalEncoding::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
				case NormalEncoding::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
				case NormalEncoding::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
				default: return DXGI_FORMAT_UNKNOWN;
			}
		}

	private:
		bool m_isAvailable{};
		NrdUserPool m_userPool{};
		NrdIntegration m_NRD;

		Device* m_device{};
		CommandBuffer* m_commandBuffer{};
		struct NriInterface : CoreInterface, HelperInterface, WrapperD3D12Interface {} m_NRI{};

		struct TextureBarrierDescEx : TextureBarrierDesc {
			ID3D12Resource* resource;
			AccessLayoutStage initial;
		};
		vector<TextureBarrierDescEx> m_textureBarrierDescs;
	};
}
