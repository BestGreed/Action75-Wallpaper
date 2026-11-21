#include "Action75Wallpaper12.h"
#include "Bath.hpp"
#include "Appearance.hpp"
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <nlohmann/json.hpp>

namespace {
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(operation);
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES description{};
    description.Type = type;
    description.CreationNodeMask = description.VisibleNodeMask = 1;
    return description;
}
D3D12_RESOURCE_DESC image(UINT width, UINT height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    return description;
}
struct Upload {
    ComPtr<ID3D12Resource> buffer;
    unsigned char* data = nullptr;

    void allocate(ID3D12Device* device, UINT64 bytes) {
        const auto memory = heap(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes;
        description.Height = description.DepthOrArraySize = description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &description,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&buffer)), "Allocate upload buffer");
        const D3D12_RANGE noRead{};
        void* mapped = nullptr;
        check(buffer->Map(0, &noRead, &mapped), "Map upload buffer");
        data = static_cast<unsigned char*>(mapped);
    }
    D3D12_GPU_VIRTUAL_ADDRESS address() const { return buffer->GetGPUVirtualAddress(); }
};
void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, 0, before, after};
    commands->ResourceBarrier(1, &barrier);
}
std::vector<char> shader(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Open bundled shader");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Studio {
    static constexpr UINT TileSide = 12;
    static constexpr UINT IndexCount = TileSide * TileSide * 6;
    static constexpr UINT InstanceCount = Bath::tilesAcross * Bath::tilesDown +
                                          2 * (Bath::tilesAcross + Bath::tilesDown);
    static constexpr UINT InstanceStride = 2 * sizeof(uint32_t);
    static constexpr UINT RowAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    static constexpr UINT SurfacePitch = (Bath::W * 4 + RowAlignment - 1) / RowAlignment * RowAlignment;
    static constexpr UINT LookOffset = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    static constexpr auto ReadSurface = static_cast<D3D12_RESOURCE_STATES>(
        UINT(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) | UINT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    Bath bath;
    std::vector<std::pair<std::string, Vec>> magnets;
    ID3D12Device* device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> floorPipeline, fluidPipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors, depthView;
    ComPtr<ID3D12Resource> surface, depth;
    Upload surfaceUpload, indices, tiles, constants;
    UINT depthWidth = 0, depthHeight = 0;

    void ensureDepth(UINT width, UINT height) {
        if (width == depthWidth && height == depthHeight) return;
        const auto memory = heap(D3D12_HEAP_TYPE_DEFAULT);
        auto description = image(width, height, DXGI_FORMAT_D16_UNORM);
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                            D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = description.Format;
        clear.DepthStencil.Depth = 1;
        check(device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &description,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                               IID_PPV_ARGS(depth.ReleaseAndGetAddressOf())),
              "Allocate liquid depth");
        device->CreateDepthStencilView(depth.Get(), nullptr, depthView->GetCPUDescriptorHandleForHeapStart());
        depthWidth = width;
        depthHeight = height;
    }

    void createPipelines(const std::filesystem::path& directory) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER bindings[3]{};
        bindings[0].ParameterType = bindings[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        bindings[1].Descriptor.ShaderRegister = 1;
        bindings[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        bindings[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bindings[2].DescriptorTable = {1, &range};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC signature{
            3, bindings, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        };
        ComPtr<ID3DBlob> encoded;
        check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &encoded, nullptr), "Serialize shader bindings");
        check(device->CreateRootSignature(0, encoded->GetBufferPointer(), encoded->GetBufferSize(),
                                          IID_PPV_ARGS(&root)), "Create shader bindings");
        const auto floorVS = shader(directory / L"floorVS.cso");
        const auto floorPS = shader(directory / L"floorPS.cso");
        const auto fluidVS = shader(directory / L"fluidVS.cso");
        const auto fluidPS = shader(directory / L"fluidPS.cso");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root.Get();
        pipeline.VS = {floorVS.data(), floorVS.size()};
        pipeline.PS = {floorPS.data(), floorPS.size()};
        pipeline.BlendState.RenderTarget[0] = {
            FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL
        };
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        pipeline.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        pipeline.DepthStencilState.FrontFace = pipeline.DepthStencilState.BackFace = {
            D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS
        };
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.DSVFormat = DXGI_FORMAT_D16_UNORM;
        pipeline.SampleDesc.Count = 1;
        check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&floorPipeline)),
              "Create ceramic pipeline");

        const D3D12_INPUT_ELEMENT_DESC origin{
            "ORIGIN", 0, DXGI_FORMAT_R32G32_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1
        };
        pipeline.InputLayout = {&origin, 1};
        pipeline.VS = {fluidVS.data(), fluidVS.size()};
        pipeline.PS = {fluidPS.data(), fluidPS.size()};
        pipeline.DepthStencilState.DepthEnable = TRUE;
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&fluidPipeline)),
              "Create liquid pipeline");
    }
    float strength=1,shine=1,tone=0,fluidStyle=0;
    float foregroundOpacity=100, backgroundOpacity=100;
    SurfaceLook fluidLook,dishLook;
    void updateLook(){
        struct Constants {float floor[2][4];float fluid[3][4];};
        static_assert(sizeof(Constants)==80);
        Constants look{};
        static constexpr SurfaceLook::RGB palette[][2]={
            {{.37f,.50f,.52f},{.74f,.82f,.79f}},
            {{.57f,.46f,.40f},{.90f,.83f,.73f}},
            {{.12f,.15f,.19f},{.34f,.40f,.44f}}
        };
        int style=tone>1.5f?2:tone>.5f?1:0;
        for(int i=0;i<2;++i){
            auto rgb=dishLook.transform(palette[style][i]);
            std::copy(rgb.begin(),rgb.end(),look.floor[i]);
        }
        static constexpr SurfaceLook::RGB metals[]={{1,1,1},{1.65f,.84f,.48f},{.55f,.90f,1.85f}};
        for(int i=0;i<3;++i){
            SurfaceLook::RGB axis{};
            axis[i]=metals[(int)fluidStyle][i];
            auto rgb=fluidLook.transform(axis);
            for(int j=0;j<3;++j)look.fluid[j][i]=rgb[j];
        }
        look.floor[0][3]=backgroundOpacity/100.f;
        look.floor[1][3]=foregroundOpacity/100.f;
        look.fluid[0][3]=fluidStyle!=0 || !fluidLook.neutral();
        std::memcpy(constants.data + LookOffset, &look, sizeof(look));
    }
    double last=-1;
    bool paused=false;
    bool surfaceReady=false;
    void settings(const char* s){
        if(!s)return;
        auto j=json::parse(s,nullptr,false);
        if(!j.is_object())return;
        auto read=[&](const char* id,float& value,float low,float high){
            auto it=j.find(id);
            if(it!=j.end() && it->is_number())value=std::clamp(it->get<float>(),low,high);
        };
        read("radius",bath.radius,0.085f,0.23f);
        read("viscosity",bath.viscosity,.35f,1.8f);
        read("tension",bath.tension,.65f,1.65f);
        read("edge",bath.edge,0,1.7f);
        read("holdMotion",bath.holdMotion,0,1.5f);
        read("holdHz",bath.holdHz,.15f,1.6f);
        read("field",strength,0.65f,1.45f);
        read("priority",bath.priority,2.0f,10.0f);
        read("follow",bath.follow,4.0f,16.0f);
        read("returnRate",bath.returnRate,3.0f,12.0f);
        read("stretch",bath.stretch,0.0f,3.2f);
        read("spikes",bath.spikes,0.0f,1.6f);
        read("spacing",bath.spacing,0.034f,0.078f);
        read("shine",shine,0.55f,1.35f);
        read("tone",tone,0,2);
        read("fluidStyle",fluidStyle,0,2);
        read("foregroundOpacity",foregroundOpacity,0,100);
        read("backgroundOpacity",backgroundOpacity,0,100);
        fluidLook.read(j,"fluid");dishLook.read(j,"dish");
        if(constants.data)updateLook();
    }
    explicit Studio(const A75Create12& config) : device(config.device) {
        settings(config.settings_json);
        createPipelines(config.pack_dir);

        const D3D12_DESCRIPTOR_HEAP_DESC shaderHeap{
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
        };
        check(device->CreateDescriptorHeap(&shaderHeap, IID_PPV_ARGS(&descriptors)),
              "Allocate surface descriptor");
        const D3D12_DESCRIPTOR_HEAP_DESC depthHeap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
        check(device->CreateDescriptorHeap(&depthHeap, IID_PPV_ARGS(&depthView)),
              "Allocate depth descriptor");
        const auto memory = heap(D3D12_HEAP_TYPE_DEFAULT);
        const auto description = image(Bath::W, Bath::H, DXGI_FORMAT_R16G16_UNORM);
        check(device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &description,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&surface)), "Allocate liquid surface");
        device->CreateShaderResourceView(surface.Get(), nullptr,
                                         descriptors->GetCPUDescriptorHandleForHeapStart());
        surfaceUpload.allocate(device, UINT64(SurfacePitch) * Bath::H);
        indices.allocate(device, IndexCount * sizeof(uint16_t));
        tiles.allocate(device, InstanceCount * InstanceStride);
        constants.allocate(device, 2 * LookOffset);
        updateLook();
        ensureDepth(config.width, config.height);

        std::array<uint16_t, IndexCount> mesh{};
        size_t index = 0;
        for (UINT y = 0; y < TileSide; ++y)
            for (UINT x = 0; x < TileSide; ++x) {
                const auto base = y * (TileSide + 1) + x;
                for (const auto offset : {0, 1, 13, 13, 1, 14})
                    mesh[index++] = static_cast<uint16_t>(base + offset);
            }
        std::memcpy(indices.data, mesh.data(), sizeof(mesh));
    }
    void event(const A75Event& e){
        if(e.type==A75_SETTINGS)settings(e.settings_json);
        if(e.type==A75_PAUSE||e.type==A75_INPUT_RESET)magnets.clear();
        if(e.type==A75_PAUSE)paused=true;
        if(e.type==A75_RESUME){paused=false;last=-1;}
        if(!paused&&(e.type==A75_KEY_DOWN||e.type==A75_KEY_UP)&&e.code){
            auto it=std::find_if(magnets.begin(),magnets.end(),[&](auto& k){return k.first==e.code;});
            if(e.type==A75_KEY_UP){if(it!=magnets.end())magnets.erase(it);}
            else if(it==magnets.end()&&magnets.size()<128){
                Vec pos{e.x*Bath::aspect,e.y+.027f};
                magnets.push_back({e.code,pos});
                // A very short tap still imparts momentum between two frame callbacks.
                bath.tap(pos,strength);
            }
        }
    }
    void uploadSurface(ID3D12GraphicsCommandList* commands) {
        bath.raster();
        const UINT x = std::min(bath.previousX0, bath.x0);
        const UINT y = std::min(bath.previousY0, bath.y0);
        const UINT width = std::max(bath.previousX1, bath.x1) + 1 - x;
        const UINT height = std::max(bath.previousY1, bath.y1) + 1 - y;
        const UINT bytes = width * 4;
        const UINT pitch = (bytes + RowAlignment - 1) / RowAlignment * RowAlignment;
        for (UINT row = 0; row < height; ++row)
            std::memcpy(surfaceUpload.data + size_t(row) * pitch,
                        bath.surface.data() + 2 * (size_t(y + row) * Bath::W + x), bytes);
        if (surfaceReady)
            transition(commands, surface.Get(), ReadSurface, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = surfaceUpload.buffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16_UNORM, width, height, 1, pitch};
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = surface.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&destination, x, y, 0, &source, nullptr);
        transition(commands, surface.Get(), D3D12_RESOURCE_STATE_COPY_DEST, ReadSurface);
        surfaceReady = true;
    }

    UINT writeTiles() {
        auto* origins = reinterpret_cast<uint32_t*>(tiles.data);
        UINT count = 0;
        const auto add = [&](UINT x, UINT y) {
            *origins++ = x;
            *origins++ = y;
            ++count;
        };
        for (int y = bath.y0 / 12 * 12; y <= bath.y1 && y < Bath::H - 1; y += 12)
            for (int x = bath.x0 / 12 * 12; x <= bath.x1 && x < Bath::W - 1; x += 12)
                if (bath.wetTiles[(y / 12) * Bath::tilesAcross + x / 12]) add(x, y);
        // The four boundary strips share their upper vertices with the surface.
        const auto closeEdge = [&](UINT side, int first, int last, int limit) {
            for (int along = first / 12 * 12; along <= last && along < limit; along += 12)
                add(Bath::W + side, along);
        };
        if (bath.x0 == 0) closeEdge(0, bath.y0, bath.y1, Bath::H - 1);
        if (bath.x1 == Bath::W - 1) closeEdge(1, bath.y0, bath.y1, Bath::H - 1);
        if (bath.y0 == 0) closeEdge(2, bath.x0, bath.x1, Bath::W - 1);
        if (bath.y1 == Bath::H - 1) closeEdge(3, bath.x0, bath.x1, Bath::W - 1);
        return count;
    }

    int draw(const A75Frame12& frame) {
        bool moving = false;
        if (!paused) {
            std::array<Vec,128> keys{};
            for (size_t i = 0; i < magnets.size(); ++i) keys[i] = magnets[i].second;
            const float dt = last < 0 ? 1.f / 60 : static_cast<float>(std::clamp(frame.seconds - last, .001, .06));
            last = frame.seconds;
            moving = bath.step(keys.data(), static_cast<int>(magnets.size()), strength, dt);
        }
        // A paused settings update redraws the original frozen surface.
        if (!paused || !surfaceReady)
            uploadSurface(frame.commands);

        float material[16] = {shine, tone, 1.f/frame.width, 1.f/frame.height, bath.x0 / 960.f - .02f, bath.y0 / 360.f - .04f,
                             bath.x1 / 960.f + .02f, bath.y1 / 360.f + .04f};
        for (int i = 0; i < std::min(bath.active, 2); ++i) {
            const auto& pole = bath.poles[bath.active - 1 - i];
            material[8 + 4 * i] = pole.pos.x;
            material[9 + 4 * i] = pole.pos.y;
            material[10 + 4 * i] = pole.stress;
            material[11 + 4 * i] = bath.radius;
        }
        std::memcpy(constants.data, material, sizeof(material));

        ensureDepth(frame.width, frame.height);
        const auto dsv = depthView->GetCPUDescriptorHandleForHeapStart();
        auto* commands = frame.commands;
        ID3D12DescriptorHeap* textureHeaps[] = {descriptors.Get()};
        commands->SetDescriptorHeaps(1, textureHeaps);
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetGraphicsRootConstantBufferView(0, constants.address());
        commands->SetGraphicsRootConstantBufferView(1, constants.address() + LookOffset);
        commands->SetGraphicsRootDescriptorTable(2, descriptors->GetGPUDescriptorHandleForHeapStart());
        commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        commands->OMSetRenderTargets(1, &frame.rtv, FALSE, &dsv);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->SetPipelineState(floorPipeline.Get());
        commands->DrawInstanced(3, 1, 0, 0);

        const auto count = writeTiles();
        if (count && foregroundOpacity > 0) {
            const D3D12_VERTEX_BUFFER_VIEW instances{tiles.address(), count * InstanceStride, InstanceStride};
            const D3D12_INDEX_BUFFER_VIEW mesh{indices.address(), IndexCount * sizeof(uint16_t), DXGI_FORMAT_R16_UINT};
            commands->IASetVertexBuffers(0, 1, &instances);
            commands->IASetIndexBuffer(&mesh);
            commands->SetPipelineState(fluidPipeline.Get());
            commands->DrawIndexedInstanced(IndexCount, count, 0, 0, 0);
        }
        if (!moving) last = -1;
        return moving ? 0 : -1;
    }
};

void* __cdecl create(const A75Create12* config) {
    if (!config || config->size < sizeof(*config) || !config->device || !config->pack_dir ||
        !config->width || !config->height) return nullptr;
    try { return new Studio(*config); }
    catch (...) { return nullptr; }
}
void __cdecl event(void* instance, const A75Event* data) {
    if (!instance || !data || data->size < sizeof(*data)) return;
    try { static_cast<Studio*>(instance)->event(*data); }
    catch (...) {}
}
int32_t __cdecl render(void* instance, const A75Frame12* frame) {
    if (!instance || !frame || frame->size < sizeof(*frame) || !frame->commands ||
        !frame->rtv.ptr || !frame->width || !frame->height) return -2;
    try { return static_cast<Studio*>(instance)->draw(*frame); }
    catch (...) { return -2; }
}
void __cdecl destroy(void* instance) { delete static_cast<Studio*>(instance); }
} // namespace

extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
    static const A75Wallpaper12 api{sizeof(api), ACTION75_WALLPAPER_ABI12, create, event, render, destroy, nullptr};
    return abi == api.abi ? &api : nullptr;
}
