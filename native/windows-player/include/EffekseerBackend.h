#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <array>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace action75 {

// The owner calls every method on its D3D11 immediate-context thread.
// Coordinates are viewport pixels, with the origin in the top-left corner.
// This adapter loads Effekseer effects, not Unreal/Finalmouse .pak files.
class EffekseerBackend final {
public:
    struct UniformValue { std::string name; unsigned component; float value; };
    EffekseerBackend();
    ~EffekseerBackend();
    EffekseerBackend(const EffekseerBackend&) = delete;
    EffekseerBackend& operator=(const EffekseerBackend&) = delete;

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    int maxParticles = 8192);
    void Shutdown();
    void Resize(unsigned width, unsigned height);

    // Returns a positive reusable effect ID, or -1 on failure. Relative texture
    // and material paths remain relative to this file's parent directory.
    int LoadEffect(const std::filesystem::path& path, bool runtimeControls = false,
                   std::size_t paletteIndex = 0, const std::filesystem::path& package = {});
    // Stops only this effect's instances, removes its cached resources and path,
    // and preserves all other loaded IDs. Useful for transactional pack swaps.
    bool UnloadEffect(int effectId);
    void UnloadEffects();

    // Returns an instance handle, or -1 on failure. One effect can be played at
    // many key positions without reloading its textures or allocating a renderer.
    int PlayAt(int effectId, float pixelX, float pixelY,
               float effectUnitsToPixels = 1.0f);
    void Stop(int instanceHandle);
    // Recycle only the oldest live burst, retaining all other paint marks.
    bool StopOldest();
    void StopAll();
    void SetDynamicInput(int instanceHandle, unsigned index, float value);
    void SetDynamicInputs(const std::array<std::optional<float>, 4>& inputs);
    std::array<float, 4> GetDynamicInputs(int instanceHandle) const;
    void SetColor(int instanceHandle, std::uint8_t red, std::uint8_t green,
                  std::uint8_t blue, std::uint8_t alpha = 255);
    // Controlled Paintball materials use independent brightness and pigment
    // concentration uniforms. Neither changes their erosion or opacity masks.
    // Other effects retain byte-color tint/opacity and scale compatibility.
    void SetAppearance(float scale, float brightness, float intensity,
                       const std::array<std::uint8_t, 3>& color, float speed = 1.0f);
    void SetColorGrade(float saturation, float hueDegrees,
                       const std::vector<std::array<std::uint8_t, 3>>& palette,
                       bool usePalette);
    bool ValidateMaterialUniforms(int effectId,const std::vector<UniformValue>& values,
                                 std::string& error) const;
    bool SetMaterialUniforms(int effectId,const std::vector<UniformValue>& values,
                            std::string& error);

    // Optional shader-readable scene copy for distortion/custom materials.
    // It must not also be bound as the active render target when Draw runs.
    void SetBackground(ID3D11ShaderResourceView* background);

    // Seconds are converted to Effekseer's 60-frame time units. No render loop,
    // window, timer, input capture or Present call is owned by this adapter.
    void Update(float elapsedSeconds);
    // Realtime frames defer simulation once every effect is fully transparent.
    // Catch up that time before counting instances for a new trigger.
    void UpdateForFrame(float elapsedSeconds);
    bool CatchUpIdle();
    bool Draw();
    bool HasActiveEffects() const;
    bool HasVisibleEffects() const;
    std::size_t ActiveEffectCount() const;
    int ActiveParticleCount() const;
    const std::string& LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace action75
