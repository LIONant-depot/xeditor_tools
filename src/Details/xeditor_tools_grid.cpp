#include "../xeditor_tools_grid.h"
#include "dependencies/xGPU/source/Tools/xgpu_xcore_bitmap_helpers.h"

namespace xeditor_tools
{
    // The grid and shadow-generation shaders every editor's own grid already compiled with (E21's
    // pair, physically relocated here from xgeom_static.plugin - it was already shared by several
    // plugins, just living in one of them by accident).
    inline constexpr std::uint32_t g_GridVertShader[] =
    {
        #include "xeditor_tools_grid_vert.h"
    };
    inline constexpr std::uint32_t g_GridFragShader[] =
    {
        #include "xeditor_tools_grid_frag.h"
    };

    // The grid shader's uniform block is too large for push constants, so it is a real (dynamic) UBO.
    struct alignas(256) grid_uniform
    {
        xmath::fmat4    m_L2W;
        xmath::fmat4    m_W2C;
        xmath::fmat4    m_ShadowL2C;
        xmath::fvec3    m_WorldSpaceCameraPos = xmath::fvec3(0.0f, 10.0f, 0.0f);
        float           m_MajorGridDiv = 10.0f;
        float           m_Pass         = 0.0f;      // 0 the fill, 1 the lines (the shaders' Pass)
        float           m_FillAlpha    = 1.0f;
        float           m_Rise         = 0.0f;      // grid::m_Rise
        float           m_JitterPx[2]  = { 0, 0 };
        float           m_LineHalo     = 0.0f;
        float           m_Adaptive     = 0.0f;      // > 0: the lines read SamplerUnder, + this wrapped (grid::m_LineContrast)
        float           m_Pad[4]       = { 0, 0, 0, 0 };   // (std140: the vec4 below starts at 256)
        float           m_UnderColor[4] = { 0, 0, 0, 0 };   // a = 1: the lines draw over this plain colour instead of SamplerUnder
    };

    static_assert(offsetof(grid_uniform, m_UnderColor) == 256, "grid_uniform must match the shader's std140 block (UnderColor at 256)");

    bool grid::Ok(xgpu::device::error* pErr) noexcept
    {
        if (!pErr) return true;
        std::printf("xeditor_tools::grid: %s\n", std::string(xgpu::getErrorMsg(pErr)).c_str());
        return false;
    }

    xmath::fmat4 grid::ClipToTextureSpace(void) noexcept
    {
        xmath::fmat4 M;
        M.setupSRT({ 0.5f, 0.5f, 1.0f }, { 0_xdeg }, { 0.5f, 0.5f, 0.0f });
        return M;
    }

    bool grid::Init(xgpu::device& Device, bool bShadow) noexcept
    {
        if (m_bReady) return true;
        m_pDevice = &Device;
        m_bShadow = bShadow;

        if (auto* pErr = xgpu::tools::bitmap::Create(m_White, Device, xbitmap::getDefaultBitmap()); pErr) return false;

        auto Attributes = std::array
        { xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(vertex, m_X), .m_Format = xgpu::vertex_descriptor::format::FLOAT_3D }
        , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(vertex, m_U), .m_Format = xgpu::vertex_descriptor::format::FLOAT_2D }
        , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(vertex, m_Color), .m_Format = xgpu::vertex_descriptor::format::UINT8_4D_NORMALIZED }
        };
        if (!Ok(Device.Create(m_GridVD, xgpu::vertex_descriptor::setup{ .m_VertexSize = sizeof(vertex), .m_Attributes = Attributes }))) return false;

        // A single quad, -1..1 in local X/Y, Z=0 - the same shape every editor's own grid used
        // (previously E19's PLANE3D primitive), just built by hand here instead of reaching into that
        // example code for one static quad.
        {
            const std::uint32_t White = 0xFFFFFFFFu;
            const vertex Verts[4] =
            { { -1, -1, 0,  0, 0, White }
            , {  1, -1, 0,  1, 0, White }
            , {  1,  1, 0,  1, 1, White }
            , { -1,  1, 0,  0, 1, White }
            };
            if (!Ok(Device.Create(m_GridVerts, { .m_Type = xgpu::buffer::type::VERTEX, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(vertex), .m_EntryCount = 6 }))) return false;
            (void)m_GridVerts.MemoryMap(0, 6, [&](void* pData)
            {
                // Two triangles, matching the quad's winding every other editor's grid already drew with
                auto* pV = static_cast<vertex*>(pData);
                pV[0] = Verts[0]; pV[1] = Verts[1]; pV[2] = Verts[2];
                pV[3] = Verts[0]; pV[4] = Verts[2]; pV[5] = Verts[3];
            });

            // An identity-ramp index buffer - the vertex buffer above is already laid out in draw order,
            // but this API's Draw() reads through whatever index buffer is currently bound (confirmed
            // live: with none ever bound here, it drew through stale indices left over from an unrelated
            // earlier draw call, rendering only one of the two triangles). Every other draw call in this
            // codebase, even a plain vertex-order list, binds one for exactly this reason.
            if (!Ok(Device.Create(m_GridIndices, { .m_Type = xgpu::buffer::type::INDEX, .m_EntryByteSize = sizeof(std::uint32_t), .m_EntryCount = 6 }))) return false;
            (void)m_GridIndices.MemoryMap(0, 6, [&](void* pData)
            {
                auto* pIndex = static_cast<std::uint32_t*>(pData);
                for (std::uint32_t i = 0; i < 6; ++i) pIndex[i] = i;
            });
        }

        if (!Ok(Device.Create(m_GridUBO, { .m_Type = xgpu::buffer::type::UNIFORM, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(grid_uniform), .m_EntryCount = 32 }))) return false;

        {
            xgpu::shader Vert, Frag;
            auto Shader = [&](xgpu::shader& Out, xgpu::shader::type::bit Type, const std::uint32_t* pCode, std::size_t nWords)
            {
                return Ok(Device.Create(Out, { .m_Type = Type, .m_Sharer = xgpu::shader::setup::raw_data{ std::span{ (std::int32_t*)pCode, nWords } } }));
            };
            if (!Shader(Vert, xgpu::shader::type::bit::VERTEX,   g_GridVertShader, std::size(g_GridVertShader))) return false;
            if (!Shader(Frag, xgpu::shader::type::bit::FRAGMENT, g_GridFragShader, std::size(g_GridFragShader))) return false;

            auto Binds    = std::array{ xgpu::pipeline::uniform_binds{ .m_BindIndex = 0, .m_Usage = { .m_bVertex = true, .m_bFragment = true }, .m_Type = xgpu::pipeline::uniform_binds::type::UBO_DYNAMIC } };
            const auto Clamp = xgpu::pipeline::sampler{ .m_AddressMode = std::array{ xgpu::pipeline::sampler::address_mode::CLAMP, xgpu::pipeline::sampler::address_mode::CLAMP, xgpu::pipeline::sampler::address_mode::CLAMP } };
            auto Samplers = std::array{ Clamp, Clamp };         // the shadow map, what the lines draw over
            auto Shaders  = std::array<const xgpu::shader*, 2>{ &Frag, &Vert };
            // The ground reads the same from both sides, so no culling: the preview's own viewport
            // convention flips the winding.
            // A see-through overlay (the fill's FillAlpha, the lines' coverage): blended, no depth written (what is drawn after it is not
            // hidden by a transparent ground). Alpha is "over" too: the host composites the view by its alpha, and where no geometry was drawn
            // it is 0 - keeping the target's alpha there hid the whole grid over the empty background
            auto Blend = xgpu::pipeline::blend::getAlphaOriginal();
            Blend.m_AlphaSrcFactor = xgpu::pipeline::blend::factor::ONE;
            Blend.m_AlphaDstFactor = xgpu::pipeline::blend::factor::ONE_MINUS_SRC_ALPHA;
            // Two pipelines, the same state: [0] the fill, [1] the lines (the shaders place each a hair off the plane: no z-fighting, see them)
            for (int i = 0; i < 2; ++i)
                if (!Ok(Device.Create(m_GridPipeline[i], xgpu::pipeline::setup{ .m_VertexDescriptor = m_GridVD, .m_Shaders = Shaders, .m_UniformBinds = Binds, .m_Samplers = Samplers
                    , .m_Primitive = { .m_Cull = xgpu::pipeline::primitive::cull::NONE }, .m_DepthStencil = { .m_bDepthWriteEnable = false }, .m_Blend = Blend }))) return false;

            if (bShadow)
            {
                if (!Ok(Device.Create(m_ShadowMap, { .m_Format = xgpu::texture::format::DEPTH_U16, .m_Width = 1024, .m_Height = 1024, .m_isGamma = false }))) return false;
                std::array<xgpu::renderpass::attachment, 1> Attachments{ m_ShadowMap };
                if (!Ok(Device.Create(m_ShadowPass, { .m_Attachments = Attachments }))) return false;
            }
            auto Bindings = std::array{ xgpu::pipeline_instance::sampler_binding{ bShadow ? m_ShadowMap : m_White }, xgpu::pipeline_instance::sampler_binding{ m_White } };
            for (int i = 0; i < 2; ++i) if (!Ok(Device.Create(m_GridInstance[i], { .m_PipeLine = m_GridPipeline[i], .m_SamplersBindings = Bindings }))) return false;
        }

        m_bReady = true;
        return true;
    }

    void grid::Release(void) noexcept
    {
        // Manual, one field at a time - xeditor::DestroyGpu (a variadic convenience wrapper) lives in
        // xLION's own source/Tools/Editor/, not in the xeditor dependency proper, so this depot can't
        // reach into it without creating a reverse dependency on the very project consuming it.
        if (!m_pDevice) return;
        for (auto& I : m_GridInstance) m_pDevice->Destroy(std::move(I));
        if (m_UnderInstance.m_Private) m_pDevice->Destroy(std::move(m_UnderInstance));
        m_pUnderKey = nullptr;
        for (auto& P : m_GridPipeline) m_pDevice->Destroy(std::move(P));
        if (m_bShadow)
        {
            m_pDevice->Destroy(std::move(m_ShadowPass));
            m_pDevice->Destroy(std::move(m_ShadowMap));
        }
        m_pDevice->Destroy(std::move(m_White));
    }

    void grid::Draw(xgpu::cmd_buffer& CmdBuffer, const xmath::fmat4& W2C, const xmath::fvec3& CameraPos, const xmath::fmat4& ShadowL2C, float YOffset, xmath::fvec2 JitterPx, const xgpu::texture* pUnder, int PassMask) noexcept
    {
        if (!m_bReady) return;
        // The lines' pipeline with what they draw over bound (only the lines read it); made again when it is another image (a resized view)
        const bool bAdaptive = m_bAdaptiveLines && pUnder && pUnder->m_Private;
        if (bAdaptive && m_pUnderKey != pUnder->m_Private.get())
        {
            if (m_UnderInstance.m_Private) m_pDevice->Destroy(std::move(m_UnderInstance));
            auto Bindings = std::array{ xgpu::pipeline_instance::sampler_binding{ m_bShadow ? m_ShadowMap : m_White }, xgpu::pipeline_instance::sampler_binding{ const_cast<xgpu::texture&>(*pUnder) } };
            m_pUnderKey = Ok(m_pDevice->Create(m_UnderInstance, { .m_PipeLine = m_GridPipeline[1], .m_SamplersBindings = Bindings })) ? static_cast<const void*>(pUnder->m_Private.get()) : nullptr;
        }
        const bool bUnder = bAdaptive && m_pUnderKey == pUnder->m_Private.get();
        const bool bPlain = m_bAdaptiveLines && !bUnder;       // no image of what is under: the previews' plain background (m_Under*)
        if (CameraPos.m_Y < YOffset) return;        // front face only: from below the ground there is no grid (Tomas 2026-11-16)

        for (int Pass = 0; Pass < 2; ++Pass)        // the fill (behind the plane), then the lines (in front of it): see Init
        {
        if (Pass == 0 && m_FillAlpha <= 0.0f) continue;
        if (!(PassMask & (1 << Pass))) continue;
        CmdBuffer.setPipelineInstance(Pass == 1 && bUnder ? m_UnderInstance : m_GridInstance[Pass]);
        CmdBuffer.setBuffer(m_GridIndices);
        CmdBuffer.setBuffer(m_GridVerts);
        auto& Uniform = m_GridUBO.allocEntry<grid_uniform>();
        Uniform.m_Pass         = float(Pass);
        Uniform.m_FillAlpha    = std::clamp(m_FillAlpha, 0.0f, 1.0f);
        Uniform.m_Rise         = m_Rise;
        Uniform.m_LineHalo     = std::clamp(m_LineHalo, 0.0f, 1.0f);
        Uniform.m_Adaptive     = Pass == 1 && (bUnder || bPlain) ? (std::abs(m_LineContrast) < 0.01f ? -0.01f : std::clamp(m_LineContrast, -0.5f, 0.5f)) : 0.0f;
        Uniform.m_UnderColor[0] = m_UnderR; Uniform.m_UnderColor[1] = m_UnderG; Uniform.m_UnderColor[2] = m_UnderB; Uniform.m_UnderColor[3] = bPlain ? 1.0f : 0.0f;
        Uniform.m_JitterPx[0]  = JitterPx.m_X; Uniform.m_JitterPx[1] = JitterPx.m_Y;
        Uniform.m_WorldSpaceCameraPos = CameraPos;
        Uniform.m_L2W          = xmath::fmat4(xmath::fvec3(100.f, 100.0f, 1.f), xmath::radian3(-90_xdeg, 0_xdeg, 0_xdeg), xmath::fvec3(0, YOffset, 0));
        Uniform.m_W2C          = W2C;
        Uniform.m_ShadowL2C    = m_bShadow ? ClipToTextureSpace() * ShadowL2C * Uniform.m_L2W : xmath::fmat4::fromZero();
        Uniform.m_MajorGridDiv = 10.0f;
        CmdBuffer.setDynamicUBO(m_GridUBO, 0);
        CmdBuffer.Draw(6);
        }
    }
}
