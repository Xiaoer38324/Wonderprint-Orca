#include "SnapmakerBufferTexture.hpp"

#include "slic3r/GUI/3DScene.hpp"

#include <GL/glew.h>

// ============================================================================
// Snapmaker render pipeline - buffer texture
// Ported from Snapmaker/OrcaSlicer GUI/BufferTexture.cpp (render engine 2.4.1).
// ============================================================================

namespace Slic3r {
namespace GUI {

namespace {

GLenum InternalFormatOf(SnapmakerBufferTexture::EFormat format)
{
    return (format == SnapmakerBufferTexture::EFormat::RG32F) ? GL_RG32F : GL_RGBA32F;
}

size_t FloatsPerTexel(SnapmakerBufferTexture::EFormat format)
{
    return (format == SnapmakerBufferTexture::EFormat::RG32F) ? 2 : 4;
}

} // namespace

SnapmakerBufferTexture::~SnapmakerBufferTexture()
{
    Reset();
}

void SnapmakerBufferTexture::Upload(const float* data, size_t floatCount, EFormat format)
{
    if (data == nullptr || floatCount == 0 || floatCount % FloatsPerTexel(format) != 0)
        return;

    if (_bufferId == 0)
        glsafe(::glGenBuffers(1, &_bufferId));
    glsafe(::glBindBuffer(GL_TEXTURE_BUFFER, _bufferId));
    glsafe(::glBufferData(GL_TEXTURE_BUFFER, GLsizeiptr(floatCount * sizeof(float)), data, GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_TEXTURE_BUFFER, 0));

    if (_textureId == 0)
        glsafe(::glGenTextures(1, &_textureId));
    // glTexBuffer() acts on the texture currently bound to the
    // GL_TEXTURE_BUFFER target of the active texture unit.
    glsafe(::glBindTexture(GL_TEXTURE_BUFFER, _textureId));
    glsafe(::glTexBuffer(GL_TEXTURE_BUFFER, InternalFormatOf(format), _bufferId));
    glsafe(::glBindTexture(GL_TEXTURE_BUFFER, 0));

    _texelCount = floatCount / FloatsPerTexel(format);
}

void SnapmakerBufferTexture::Upload(const std::vector<float>& data, EFormat format)
{
    Upload(data.data(), data.size(), format);
}

void SnapmakerBufferTexture::Bind(unsigned int stage) const
{
    if (_textureId == 0)
        return;

    glsafe(::glActiveTexture(GL_TEXTURE0 + stage));
    glsafe(::glBindTexture(GL_TEXTURE_BUFFER, _textureId));
}

void SnapmakerBufferTexture::Reset()
{
    // The buffers may outlive the GL context during shutdown; deleting a
    // never-created id (0) would be harmless but is avoided anyway.
    if (_bufferId != 0) {
        glsafe(::glDeleteBuffers(1, &_bufferId));
        _bufferId = 0;
    }
    if (_textureId != 0) {
        glsafe(::glDeleteTextures(1, &_textureId));
        _textureId = 0;
    }
    _texelCount = 0;
}

} // namespace GUI
} // namespace Slic3r
