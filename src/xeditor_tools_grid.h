#ifndef XEDITOR_TOOLS_GRID_H
#define XEDITOR_TOOLS_GRID_H
#pragma once

// The ground grid every 3D editor viewport in this codebase already drew, and the shadow map it can
// receive a shadow from - pulled out of xskeleton_editor_scene.h's own identical setup (already
// informally shared by the skeleton/skin/anim editors through inheritance) so every editor, including
// ones with no reason to depend on a skeleton plugin (GeomStatic, the Level Editor), can share it too.
//
// Deliberately does NOT own a shadow-CASTER pipeline: casting is resource-type specific (a mesh's
// triangles, a skinned mesh's deformed triangles, a skeleton's bone wedges all need their own vertex
// data and shader), so a consumer draws its own geometry into m_ShadowPass with its own pipeline, the
// same shape every editor already used - this class only owns the shared shadow render target itself
// and the ground plane that samples it.
#include "dependencies/xGPU/source/xgpu.h"

namespace xeditor_tools
{
    class grid
    {
    public:
        xgpu::texture       m_ShadowMap;    // valid only when Init was called with bShadow=true
        xgpu::renderpass    m_ShadowPass;   // ditto - a consumer's own shadow-caster pipeline draws into this
        xgpu::texture       m_White;        // 1x1 white texture, handy for anything that needs a default
        bool                m_bReady        = false;
        bool                m_bShadow       = false;
        xgpu::device*       m_pDevice       = nullptr;

        bool Init(xgpu::device& Device, bool bShadow) noexcept;

        // The pipeline/white texture go back to the device (a dropped one crashes when the device
        // empties its queue); buffers drop by themselves.
        void Release(void) noexcept;

        // The ground plane, centered at world X/Z zero so it always shows where the origin is. YOffset
        // moves it up/down (GeomStatic's own "align grid to the geometry's lowest point" toggle needs
        // this; every other consumer just leaves it at the default 0, world origin). ShadowL2C is
        // world-space-to-the-light's-clip-space (pass xmath::fmat4::fromZero() for "no shadow" - the
        // shader reads an all-zero matrix as "always lit", the same convention every shadow-casting
        // editor already used).
        void Draw(xgpu::cmd_buffer& CmdBuffer, const xmath::fmat4& W2C, const xmath::fvec3& CameraPos, const xmath::fmat4& ShadowL2C, float YOffset = 0.0f) noexcept;

        // Clip space to shadow-texture space - the remap every shadow matrix fed to a shader goes
        // through. Shared here since every shadow-casting editor already needed the identical helper.
        static xmath::fmat4 ClipToTextureSpace(void) noexcept;

        // Matches the grid shader's vertex layout exactly (position/UV/normalized-byte-color) - a local
        // copy instead of reaching into xGPU's own Examples/E19_MaterialEditor for e19::draw_vert: that
        // header lives in example code the engine side has already flagged for eventual removal, which
        // this depot has no reason to depend on for three fields. Public (with its own ready-made vertex
        // descriptor, m_GridVD) since it is a genuinely generic "textured, tinted" layout other simple
        // pipelines reuse rather than declaring their own identical one - xskeleton.plugin's own bone-
        // wedge fill pipeline does exactly this.
        struct vertex { float m_X, m_Y, m_Z, m_U, m_V; std::uint32_t m_Color; };
        xgpu::vertex_descriptor m_GridVD;

    private:
        static bool Ok(xgpu::device::error* pErr) noexcept;

        xgpu::buffer            m_GridVerts;
        xgpu::buffer            m_GridUBO;
        xgpu::pipeline          m_GridPipeline;
        xgpu::pipeline_instance m_GridInstance;
    };
}

#endif // XEDITOR_TOOLS_GRID_H
