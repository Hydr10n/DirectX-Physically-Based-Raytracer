module;

#include <filesystem>

#include "directx/d3d12.h"

#include "DirectXTexEXR.h"

export module TextureHelpers;

export import Texture;

import CommandList;
import DeviceContext;
import ErrorHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace std;
using namespace std::filesystem;

#define LOAD_FROM_MEMORY(Loader, forceSRGB, ...) \
	ScratchImage image; \
	ThrowIfFailed(Loader(reinterpret_cast<const std::byte*>(pData), size, __VA_ARGS__, nullptr, image)); \
	return LoadTexture(commandList, image, forceSRGB);

#define LOAD_FROM_FILE(Loader, forceSRGB, ...) \
	ScratchImage image; \
	ThrowIfFailed(Loader(filePath.c_str(), __VA_ARGS__, nullptr, image), filePath.string()); \
	return LoadTexture(commandList, image, forceSRGB);

export namespace DirectX::TextureHelpers {
	unique_ptr<Texture> LoadTexture(CommandList& commandList, const ScratchImage& image, bool forceSRGB = false) {
		const auto& deviceContext = commandList.GetDeviceContext();

		vector<D3D12_SUBRESOURCE_DATA> subresourceData;
		ThrowIfFailed(PrepareUpload(deviceContext, image.GetImages(), image.GetImageCount(), image.GetMetadata(), subresourceData));

		const auto& metadata = image.GetMetadata();
		TextureDimension dimension;
		switch (metadata.dimension) {
			case TEX_DIMENSION_TEXTURE1D: dimension = TextureDimension::_1; break;
			case TEX_DIMENSION_TEXTURE2D: dimension = TextureDimension::_2; break;
			case TEX_DIMENSION_TEXTURE3D: dimension = TextureDimension::_3; break;
		}
		auto texture = make_unique<Texture>(
			deviceContext,
			Texture::CreationDesc{
				.Format = forceSRGB ? MakeSRGB(metadata.format) : metadata.format,
				.Dimension = dimension,
				.Width = static_cast<UINT>(metadata.width),
				.Height = static_cast<UINT>(metadata.height),
				.DepthOrArraySize = static_cast<UINT16>(dimension == TextureDimension::_3 ? metadata.depth : metadata.arraySize),
				.MipLevels = static_cast<UINT16>(metadata.mipLevels)
			}
		);

		commandList.Copy(*texture, subresourceData);
		commandList.SetState(*texture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		return texture;
	}

	unique_ptr<Texture> LoadDDS(
		CommandList& commandList,
		const void* pData, size_t size,
		bool forceSRGB = false, DDS_FLAGS flags = DDS_FLAGS_NONE
	) {
		LOAD_FROM_MEMORY(LoadFromDDSMemoryEx, forceSRGB, flags, nullptr);
	}

	unique_ptr<Texture> LoadDDS(
		CommandList& commandList,
		const path& filePath,
		bool forceSRGB = false, DDS_FLAGS flags = DDS_FLAGS_NONE
	) {
		LOAD_FROM_FILE(LoadFromDDSFileEx, forceSRGB, flags, nullptr);
	}

	unique_ptr<Texture> LoadWIC(CommandList& commandList, const void* pData, size_t size, WIC_FLAGS flags = WIC_FLAGS_NONE) {
		LOAD_FROM_MEMORY(LoadFromWICMemory, false, flags);
	}

	unique_ptr<Texture> LoadWIC(CommandList& commandList, const path& filePath, WIC_FLAGS flags = WIC_FLAGS_NONE) {
		LOAD_FROM_FILE(LoadFromWICFile, false, flags);
	}

	unique_ptr<Texture> LoadHDR(CommandList& commandList, const void* pData, size_t size) {
		LOAD_FROM_MEMORY(LoadFromHDRMemory, false);
	}

	unique_ptr<Texture> LoadHDR(CommandList& commandList, const path& filePath) {
		LOAD_FROM_FILE(LoadFromHDRFile, false);
	}

	unique_ptr<Texture> LoadEXR(CommandList& commandList, const path& filePath) {
		LOAD_FROM_FILE(LoadFromEXRFile, false);
	}

	unique_ptr<Texture> LoadTGA(CommandList& commandList, const void* pData, size_t size, TGA_FLAGS flags = TGA_FLAGS_NONE) {
		LOAD_FROM_MEMORY(LoadFromTGAMemory, false, flags);
	}

	unique_ptr<Texture> LoadTGA(CommandList& commandList, const path& filePath, TGA_FLAGS flags = TGA_FLAGS_NONE) {
		LOAD_FROM_FILE(LoadFromTGAFile, false, flags);
	}

	unique_ptr<Texture> LoadTexture(
		CommandList& commandList,
		string_view format, const void* pData, size_t size,
		bool forceSRGBIfNecessary = false
	) {
		if (empty(format)) throw invalid_argument("Unknown texture format");

		const auto _format = data(format);
		auto texture =
			!_stricmp(_format, "dds") ? LoadDDS(commandList, pData, size, forceSRGBIfNecessary) :
			!_stricmp(_format, "hdr") ? LoadHDR(commandList, pData, size) :
			!_stricmp(_format, "tga") ? LoadTGA(commandList, pData, size, forceSRGBIfNecessary ? TGA_FLAGS_DEFAULT_SRGB : TGA_FLAGS_NONE) :
			LoadWIC(commandList, pData, size, forceSRGBIfNecessary ? WIC_FLAGS_DEFAULT_SRGB : WIC_FLAGS_NONE);
		return texture;
	}

	unique_ptr<Texture> LoadTexture(CommandList& commandList, const path& filePath, bool forceSRGBIfNecessary = false) {
		if (empty(filePath)) throw invalid_argument("Texture file path cannot be empty");

		const auto filePathExtension = filePath.extension();
		if (empty(filePathExtension)) throw invalid_argument(format("{}: Unknown file format", filePath.string()));

		const auto extension = filePathExtension.c_str() + 1;
		auto texture =
			!_wcsicmp(extension, L"dds") ? LoadDDS(commandList, filePath, forceSRGBIfNecessary) :
			!_wcsicmp(extension, L"hdr") ? LoadHDR(commandList, filePath) :
			!_wcsicmp(extension, L"exr") ? LoadEXR(commandList, filePath) :
			!_wcsicmp(extension, L"tga") ? LoadTGA(commandList, filePath, forceSRGBIfNecessary ? TGA_FLAGS_DEFAULT_SRGB : TGA_FLAGS_NONE) :
			LoadWIC(commandList, filePath, forceSRGBIfNecessary ? WIC_FLAGS_DEFAULT_SRGB : WIC_FLAGS_NONE);
		return texture;
	}
}
