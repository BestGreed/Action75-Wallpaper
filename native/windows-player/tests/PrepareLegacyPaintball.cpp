#include <Effekseer.h>
#include <Effekseer/Material/Effekseer.CompiledMaterial.h>
#include <DirectX11/EffekseerMaterialCompilerDX11.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;
using namespace Effekseer;

void compile(const fs::path &source, const fs::path &destination) {
  std::ifstream input(source, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot read material: " + source.u8string());
  std::vector<uint8_t> raw{std::istreambuf_iterator<char>(input), {}};
  MaterialFile material;
  if (raw.empty() || !material.Load(raw.data(), static_cast<int32_t>(raw.size())))
    throw std::runtime_error("Invalid material: " + source.u8string());
  MaterialCompilerDX11 compiler;
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
    throw std::runtime_error("Empty DX11 shader: " + source.u8string());
  CompiledMaterial cache;
  cache.UpdateData(shader(T::Standard, true), shader(T::Standard, false), shader(T::Model, true),
                   shader(T::Model, false), shader(T::Refraction, true),
                   shader(T::Refraction, false), shader(T::RefractionModel, true),
                   shader(T::RefractionModel, false), CompiledMaterialPlatformType::DirectX11);
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
    fs::create_directories(output / "materials");
    for (const auto &file : fs::directory_iterator(pack / "materials")) {
      if (file.path().extension() == L".efkmat")
        compile(file.path(), output / "materials" / (file.path().filename().wstring() + L"d"));
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
