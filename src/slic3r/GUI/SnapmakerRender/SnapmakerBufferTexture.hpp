#ifndef slic3r_SnapmakerBufferTexture_hpp_
#define slic3r_SnapmakerBufferTexture_hpp_

// ============================================================================
// Snapmaker render pipeline - buffer texture
// ----------------------------------------------------------------------------
// Ported from Snapmaker OrcaSlicer (Snapmaker/OrcaSlicer, "Render engine build
// - v2.4.1", GUI/BufferTexture.*). This file keeps the original Snapmaker
// design: a thin wrapper around an OpenGL texture buffer object used by the
// de-geometrized toolpath pipeline to feed raw path data to the shaders.
//
// Changes for Wonderprint-Orca:
//   * renamed into the Snapmaker* file/class namespace so the pipeline is
//     identifiable at every call site,
//   * no functional change.
// ============================================================================

#include <cstddef>
#include <vector>

namespace Slic3r {
namespace GUI {

// Thin wrapper around an OpenGL texture buffer object (TBO): a linear float
// array uploaded once and sampled by shaders with texelFetch() through a
// samplerBuffer. Used by the Snapmaker GPU path pipeline to feed raw gcode
// path data (positions, widths, per-move attributes, segment records) to the
// shaders.
class SnapmakerBufferTexture
{
public:
    enum class EFormat : unsigned char
    {
        RGBA32F, // 4 floats per texel
        RG32F    // 2 floats per texel
    };

    SnapmakerBufferTexture() = default;
    ~SnapmakerBufferTexture();

    SnapmakerBufferTexture(const SnapmakerBufferTexture&) = delete;
    SnapmakerBufferTexture& operator=(const SnapmakerBufferTexture&) = delete;
    SnapmakerBufferTexture(SnapmakerBufferTexture&&) = delete;
    SnapmakerBufferTexture& operator=(SnapmakerBufferTexture&&) = delete;

    // (Re)uploads the given floats to the buffer; must be called with an
    // active GL context. floatCount must be a multiple of the texel size.
    void Upload(const float* data, size_t floatCount, EFormat format);
    void Upload(const std::vector<float>& data, EFormat format);

    // Binds the texture to the given texture unit, as samplerBuffer target.
    // The caller is responsible for restoring GL_TEXTURE0 afterwards.
    void Bind(unsigned int stage) const;

    // Releases the GL objects; safe to call even if nothing was uploaded.
    void Reset();

    bool IsValid() const { return _textureId != 0; }
    size_t TexelCount() const { return _texelCount; }

private:
    unsigned int _textureId{ 0 };
    unsigned int _bufferId{ 0 };
    size_t _texelCount{ 0 };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SnapmakerBufferTexture_hpp_
