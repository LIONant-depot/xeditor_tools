#ifndef XEDITOR_TOOLS_CAMERA_H
#define XEDITOR_TOOLS_CAMERA_H
#pragma once

// The orbit camera every 3D editor viewport in this codebase already used, independently duplicated
// per plugin (xskeleton.plugin's own scene, xgeom_static.plugin's own render_settings, ...) - pulled
// out once here so every editor shares the same behavior and the same bug fixes. Right-drag rotates,
// middle-drag pans, wheel zooms - unchanged from every editor's own prior implementation - extended
// with an always-on WASD/QE fly (see xeditor_tools_camera.cpp's own top comment for why it is not
// gated behind a held mouse button, and how it composes with orbit instead of replacing it).
#include "dependencies/xGPU/source/tools/xgpu_view.h"

struct ImVec2;

namespace xeditor_tools
{
    class camera
    {
    public:
        xgpu::tools::view   m_View;
        xmath::radian3      m_Angles;
        float               m_Distance  = -1;                     // -1 until the first frame framed the subject
        xmath::fvec3        m_Target    = xmath::fvec3(0, 0, 0);
        bool                m_bReframe  = true;
        float               m_Radius    = 1.0f;                   // the subject: what Recenter/first-frame frames to, and what sizes the near/far depth range
        xmath::fvec3        m_Center    = xmath::fvec3(0, 0, 0);

        // World units/second, scaled by m_Distance in HandleInput so flying feels the same relative
        // speed whether zoomed in on a bolt or looking at a whole level.
        float               m_FlySpeed  = 1.0f;

        // Reframes on the next UpdateView call (e.g. after loading a new subject, or the panel's own
        // "Recenter" button).
        void Recenter(void) noexcept { m_bReframe = true; }

        // Right drag turns the camera, middle drag pans, the wheel zooms, WASD+QE flies (see the .cpp).
        // Call right after the viewport item was submitted, same as every editor already does
        // (ImGui::IsItemHovered()/IsItemActive() gates this to the panel that owns it).
        void HandleInput(void) noexcept;

        // Sizes the view to the panel, frames the subject when asked to, and places the camera. Min is
        // the panel's own ABSOLUTE on-screen rect (matching every editor's own prior UpdateView calls
        // byte for byte - RayFromScreen takes its own Viewport.Min into account internally).
        void UpdateView(const ImVec2& Min, float ViewW, float ViewH) noexcept;
    };
}

#endif // XEDITOR_TOOLS_CAMERA_H
