#pragma once
// ── PlanetTextureLoader.hpp ───────────────────────────────────────────────────
// Loads JPG/PNG files from disk into D3D11 Texture2D + ShaderResourceView using
// the Windows Imaging Component (WIC). No extra dependencies — WIC ships with
// Windows Vista+ and is already linked via windowscodecs.lib.
//
// Usage:
//   ComPtr<ID3D11ShaderResourceView> srv;
//   bool ok = LoadTextureFromFile(device, ctx, L"Grass_1K-JPG_Color.jpg", srv);

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

// ── LoadTextureFromFile ───────────────────────────────────────────────────────
// Loads an image file (JPG, PNG, BMP, TIFF …) via WIC and uploads it to the GPU
// as a 2D texture with full mip-chain.
//
// Parameters:
//   device   – D3D11 device used to create the texture and SRV
//   ctx      – immediate context used to upload pixel data (UpdateSubresource)
//   path     – wide-string path to the image file (relative to the EXE)
//   outSRV   – receives the created ShaderResourceView on success
//
// Returns true on success, false if any WIC or D3D call fails.
// On failure, outSRV is left unchanged (nullptr or previous value).
inline bool LoadTextureFromFile(ID3D11Device*           device,
                                ID3D11DeviceContext*     ctx,
                                const wchar_t*           path,
                                ComPtr<ID3D11ShaderResourceView>& outSRV)
{
    // ── Initialise WIC factory ────────────────────────────────────────────────
    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wicFactory.GetAddressOf()));
    if (FAILED(hr)) return false;

    // ── Decode the image file ─────────────────────────────────────────────────
    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(
        path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        decoder.GetAddressOf());
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Convert to 32-bit RGBA (DXGI_FORMAT_R8G8B8A8_UNORM) ─────────────────
    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    // ── Query dimensions ──────────────────────────────────────────────────────
    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    if (width == 0 || height == 0) return false;

    // ── Copy pixels to a CPU buffer ───────────────────────────────────────────
    const UINT rowPitch   = width * 4;   // 4 bytes per pixel (RGBA8)
    const UINT imageBytes = rowPitch * height;

    std::vector<uint8_t> pixels(imageBytes);
    hr = converter->CopyPixels(nullptr, rowPitch, imageBytes, pixels.data());
    if (FAILED(hr)) return false;

    // ── Compute mip levels ────────────────────────────────────────────────────
    // Full mip chain: log2(max(w,h)) + 1 levels. The GPU generates them via
    // GenerateMips() after upload.
    UINT mipLevels = 1;
    {
        UINT m = std::max(width, height);
        while (m > 1) { m >>= 1; mipLevels++; }
    }

    // ── Create the D3D11 texture ──────────────────────────────────────────────
    D3D11_TEXTURE2D_DESC td{};
    td.Width              = width;
    td.Height             = height;
    td.MipLevels          = mipLevels;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_DEFAULT;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags          = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&td, nullptr, tex.GetAddressOf());
    if (FAILED(hr)) return false;

    // Upload the top-level mip (mip 0) via UpdateSubresource
    ctx->UpdateSubresource(tex.Get(), 0, nullptr, pixels.data(), rowPitch, imageBytes);

    // ── Create the SRV ────────────────────────────────────────────────────────
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                    = td.Format;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = mipLevels;
    srvd.Texture2D.MostDetailedMip = 0;

    hr = device->CreateShaderResourceView(tex.Get(), &srvd, outSRV.GetAddressOf());
    if (FAILED(hr)) return false;

    // ── Auto-generate mipmaps ─────────────────────────────────────────────────
    ctx->GenerateMips(outSRV.Get());

    return true;
}

// ── LoadNormalMap ─────────────────────────────────────────────────────────────
// Identical to LoadTextureFromFile but keeps the texture as sRGB=false
// (normals must NOT be gamma-corrected — they are linear data stored in UNORM).
// For colour textures, use DXGI_FORMAT_R8G8B8A8_UNORM_SRGB instead.
inline bool LoadColorTextureFromFile(ID3D11Device*        device,
                                     ID3D11DeviceContext* ctx,
                                     const wchar_t*       path,
                                     ComPtr<ID3D11ShaderResourceView>& outSRV)
{
    // WIC decode (same as above)
    ComPtr<IWICImagingFactory> wicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory.GetAddressOf()))))
        return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory->CreateFormatConverter(converter.GetAddressOf()))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    if (!width || !height) return false;

    const UINT rowPitch   = width * 4;
    const UINT imageBytes = rowPitch * height;
    std::vector<uint8_t> pixels(imageBytes);
    if (FAILED(converter->CopyPixels(nullptr, rowPitch, imageBytes, pixels.data()))) return false;

    UINT mipLevels = 1;
    { UINT m = std::max(width, height); while (m > 1) { m >>= 1; mipLevels++; } }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = width;
    td.Height           = height;
    td.MipLevels        = mipLevels;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // gamma-correct colour
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags        = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, nullptr, tex.GetAddressOf()))) return false;
    ctx->UpdateSubresource(tex.Get(), 0, nullptr, pixels.data(), rowPitch, imageBytes);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = mipLevels;
    srvd.Texture2D.MostDetailedMip = 0;

    if (FAILED(device->CreateShaderResourceView(tex.Get(), &srvd, outSRV.GetAddressOf()))) return false;
    ctx->GenerateMips(outSRV.Get());
    return true;
}
