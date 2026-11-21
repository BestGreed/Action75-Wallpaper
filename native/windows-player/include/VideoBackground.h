#pragma once

#include <d3d11.h>
#include <filesystem>
#include <memory>
#include <string>

namespace action75 {

// All methods belong on the render/COM-initialized thread. Initialize is cheap:
// Media Foundation starts only in Open. UpdateFrame must run regularly while
// IsAnimated is true, including during asynchronous loading. TransferVideoFrame
// may change D3D context state; rebind the renderer's pipeline before drawing.
class VideoBackground {
public:
  VideoBackground();
  ~VideoBackground();
  VideoBackground(const VideoBackground &) = delete;
  VideoBackground &operator=(const VideoBackground &) = delete;

  bool Initialize(ID3D11Device *device, ID3D11DeviceContext *context);
  bool Open(const std::filesystem::path &file, bool preserveAspect = false);
  bool Resize(unsigned width, unsigned height);
  bool UpdateFrame();
  ID3D11ShaderResourceView *TextureView() const noexcept;
  bool IsAnimated() const noexcept;
  float AspectRatio() const noexcept;
  void SetPaused(bool paused);
  void Close() noexcept;
  const std::string &LastError() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace action75
