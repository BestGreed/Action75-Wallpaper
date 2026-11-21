#include <Effekseer.h>
#include <Effekseer/Material/Effekseer.CompiledMaterial.h>
#include <DirectX12/EffekseerMaterialCompilerDX12.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>

namespace fs = std::filesystem;
using namespace Effekseer;

std::vector<uint8_t> read(const fs::path &file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

void compile(const fs::path &source, const fs::path &destination) {
  auto raw = read(source);
  MaterialFile material;
  if (raw.empty() || !material.Load(raw.data(), static_cast<int32_t>(raw.size())))
    throw std::runtime_error("Invalid material: " + source.u8string());
  MaterialCompilerDX12 compiler;
  auto binary = CreateUniqueReference(compiler.Compile(&material));
  if (!binary)
    throw std::runtime_error("Material compilation failed: " + source.u8string());
  auto shader = [&](MaterialShaderType type, bool vertex) {
    const auto size = vertex ? binary->GetVertexShaderSize(type) : binary->GetPixelShaderSize(type);
    const auto *data =
        vertex ? binary->GetVertexShaderData(type) : binary->GetPixelShaderData(type);
    if (!size)
      return std::vector<uint8_t>{};
    return std::vector<uint8_t>(data, data + size);
  };
  using T = MaterialShaderType;
  if (!binary->GetVertexShaderSize(T::Standard) || !binary->GetPixelShaderSize(T::Standard))
    throw std::runtime_error("Empty DX12 shader: " + source.u8string());

  // Effekseer's cache format holds separate platform chunks. Keep the existing
  // chunks for the same source so the player's legacy material tests can still
  // read their original shaders. The ABI 2 renderer selects DirectX12.
  CompiledMaterial cache;
  const auto previous = read(source.wstring() + L"d");
  if (!previous.empty()) {
    CompiledMaterial existing;
    if (existing.Load(previous.data(), static_cast<int32_t>(previous.size())) &&
        existing.GetOriginalData() == raw)
      cache = std::move(existing);
  }
  cache.UpdateData(shader(T::Standard, true), shader(T::Standard, false), shader(T::Model, true),
                   shader(T::Model, false), shader(T::Refraction, true),
                   shader(T::Refraction, false), shader(T::RefractionModel, true),
                   shader(T::RefractionModel, false), CompiledMaterialPlatformType::DirectX12);
  std::vector<uint8_t> bytes;
  cache.Save(bytes, material.GetGUID(), raw);
  std::ofstream out(destination, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  if (!out)
    throw std::runtime_error("Could not write material: " + destination.u8string());
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 3)
    return 2;
  try {
    const fs::path pack = argv[1], output = argv[2];
    fs::create_directories(output);
    for (const auto &file : fs::directory_iterator(pack)) {
      if (file.path().extension() == L".efkmat")
        compile(file.path(), output / (file.path().filename().wstring() + L"d"));
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
