#pragma once
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>
namespace soundings {
inline void checked(HRESULT result,const char* message) {if(FAILED(result)) throw std::runtime_error(message);}
struct Image {UINT width=0,height=0;std::vector<std::uint8_t> rgba;};
inline Image readImage(const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> convert;
    checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Create PNG decoder");
    checked(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"Open original PNG");
    checked(decoder->GetFrame(0,&frame),"Read original PNG frame");Image image;
    checked(frame->GetSize(&image.width,&image.height),"Read original PNG size");
    if(!image.width||!image.height||image.width>8192||image.height>8192) throw std::runtime_error("PNG dimensions exceed limit");
    checked(factory->CreateFormatConverter(&convert),"Create RGBA converter");
    checked(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Convert PNG RGBA");
    image.rgba.resize(std::size_t(image.width)*image.height*4);
    checked(convert->CopyPixels(nullptr,image.width*4,UINT(image.rgba.size()),image.rgba.data()),"Decode original PNG pixels");
    return image;
}
inline void writeImage(const std::filesystem::path& path,UINT width,UINT height,const std::vector<std::uint8_t>& rgba) {
    using Microsoft::WRL::ComPtr;
    std::filesystem::create_directories(path.parent_path());
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;
    checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Create PNG encoder");
    checked(factory->CreateStream(&stream),"Create PNG stream");checked(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"Open output PNG");
    checked(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"Create PNG encoder");
    checked(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Initialize PNG encoder");
    checked(encoder->CreateNewFrame(&frame,&properties),"Create PNG frame");checked(frame->Initialize(properties.Get()),"Initialize PNG frame");
    checked(frame->SetSize(width,height),"Set PNG size");auto format=GUID_WICPixelFormat32bppRGBA;
    checked(frame->SetPixelFormat(&format),"Set PNG pixel format");
    checked(frame->WritePixels(height,width*4,UINT(rgba.size()),const_cast<BYTE*>(rgba.data())),"Write PNG pixels");
    checked(frame->Commit(),"Commit PNG frame");checked(encoder->Commit(),"Commit PNG");
}
}
