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
#include "dependencies/xGPU/source/xGPU.h"

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
        float               m_FillAlpha     = 0.57f;    // the grey fill's opacity: 0 shows what is below the ground, the lines stay
        float               m_LineHalo      = 0.11f;    // 0..1: a light outline around the dark lines, so they read on surfaces of their own colour;
                                                        // defaults (fill .57, outline .11, 1.3 cm): Tomas 2026-11-19 14:00Z, his tuned values
        int                 m_AdaptiveMode   = 1;       // 1 continuous, 2 the ground + a step, wrapped (mod 1: Tomas 07:26Z), 0 off; only with m_bAdaptiveLines
        bool                m_bAdaptiveLines = true;    // with what the lines draw over (Draw's pUnder): dark lines on light pixels, light on dark ones
                                                        // (Tomas 2026-11-23, option A: the lines are seen on any colour); off: the fixed grey lines + LineHalo
        float               m_Rise          = 0.013f;   // metres: a surface this close to the plane counts as on it (lines shown, fill hidden); a floor saved
                                                        // 6 mm up (JointTest) hid the lines near and showed them far with any per-pixel slack
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
        void Draw(xgpu::cmd_buffer& CmdBuffer, const xmath::fmat4& W2C, const xmath::fvec3& CameraPos, const xmath::fmat4& ShadowL2C, float YOffset = 0.0f, xmath::fvec2 JitterPx = { 0.0f, 0.0f }, const xgpu::texture* pUnder = nullptr) noexcept;
        // JitterPx: the TAA jitter, in pixels, of the depth the grid is tested against (xlionrender's OverlayJitter times half the view's size);
        // zero for a depth drawn without one
        // pUnder: the image the grid draws over, the view's size (xlionrender's OverlaySource); null: the fixed grey lines

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
        xgpu::buffer            m_GridIndices;  // identity ramp (0,1,2,3,4,5) - every draw call in this codebase binds an index buffer, even for a plain vertex-order list
        xgpu::buffer            m_GridUBO;
        xgpu::pipeline          m_GridPipeline[2];     // [0] the fill, biased behind the plane; [1] the lines, biased in front (grid.cpp Init)
        xgpu::pipeline_instance m_GridInstance[2];
        xgpu::pipeline_instance m_UnderInstance;        // the lines' pipeline with pUnder bound (made again when pUnder is another image)
        const void*             m_pUnderKey     = nullptr;
    };
}

#endif // XEDITOR_TOOLS_GRID_H
