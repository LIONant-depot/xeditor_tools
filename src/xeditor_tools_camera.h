#ifndef XEDITOR_TOOLS_CAMERA_H
#define XEDITOR_TOOLS_CAMERA_H
#pragma once

// The camera every 3D editor viewport in this codebase shares (the level, the skeleton / skin / anim package scene, the static geometry preview, the
// material previews), with two modes; F switches between them:
//
//   ORBIT  right drag turns the camera around its target, middle drag moves the target, the wheel changes the distance to it, W A S D Q E (right mouse
//          held) move the target. The target is drawn (xeditor_tools::DrawCameraOverlay) so the user sees what the camera turns around.
//   FLY    (Godot, Unity, Unreal) right drag turns the camera where it is, W A S D Q E (right mouse held) move it at m_Speed meters per second (Shift: x3),
//          middle drag slides it (at that same speed: a pixel is m_Speed / 500 meters), the wheel changes m_Speed (the HUD shows what it is becoming).
//
// Both modes keep the same numbers (angles, distance, target), so F never moves the camera: the eye of the fly camera is where the orbit camera is, and the
// target is the point `distance` ahead of it. The numbers live in the host (the preview's settings, a scene...): camera_rig points at them, and camera
// (below) owns them for the hosts that have no numbers of their own.
#include "dependencies/xGPU/source/Tools/xgpu_view.h"

#include <cstdint>

struct ImVec2;

namespace xeditor_tools
{
    enum class camera_mode : std::uint8_t { ORBIT, FLY };

    // What the camera remembers apart from where it is: not saved, never part of a resource
    struct camera_state
    {
        camera_mode m_Mode          = camera_mode::ORBIT;
        float       m_Speed         = 5.0f;         // FLY: meters per second
        xmath::fvec3 m_Subject      = xmath::fvec3(0, 0, 0);   // what the view is about (the center of the bounding box), set by the host: the FOV keeps its size on the screen
        bool        m_bSubject      = false;
        float       m_SubjectRadius = 0.0f;         // its size: the orbit camera does not come closer to its target than a tenth of it (0: unknown, half a meter)
        xmath::fvec3 m_Velocity     = xmath::fvec3(0, 0, 0);   // meters per second of the rig (W A S D Q E speed up and slow down: a little momentum)
        float       m_ZoomLeft      = 0.0f;         // ORBIT: what the wheel asked and the distance has not done yet (log of a ratio): the zoom glides
        float       m_LastFovDeg    = 0.0f;         // the field of view the last frame had (0: none yet)
        float       m_FovDeg        = 45.0f;        // the vertical field of view, in degrees (the camera sets it on its view every frame: the toolbar has the slider)
        float       m_SpeedFrom     = 5.0f;         // the speed before the last wheel step (the HUD shows the change)
        double      m_SpeedChanged  = -100.0;       // ImGui time of the last wheel step
        double      m_ModeChanged   = -100.0;       // ImGui time of the last F
        bool        m_bFrame        = false;        // set by F in FLY: the host frames its subject (the center of its bounding box) and the camera is in ORBIT around it
        double      m_ZoomTime      = -100.0;       // ImGui time of the last wheel step in ORBIT (the target shows which way the camera went)
        float       m_ZoomDir       = 0.0f;         // +1: closer, -1: farther
        double      m_Active        = -100.0;       // ImGui time the user last turned / moved / zoomed it (the target is drawn bright for a moment after)

        // F (and the mode button): ORBIT goes to FLY; FLY frames the subject and goes to ORBIT around it
        void SwitchMode(double Now) noexcept
        {
            if (m_Mode == camera_mode::FLY) { m_Mode = camera_mode::ORBIT; m_bFrame = true; }
            else                            { m_Mode = camera_mode::FLY; }
            m_ModeChanged = Now;
        }
    };

    // The numbers of a camera that a host owns
    struct camera_rig
    {
        camera_state&           m_State;
        xgpu::tools::view&      m_View;
        xmath::radian3&         m_Angles;
        float&                  m_Distance;
        xmath::fvec3&           m_Target;
        float                   m_OrbitFlySpeed = 1.0f;     // ORBIT: W A S D move the target m_OrbitFlySpeed * distance per second
    };

    // Call right after the viewport item was submitted (ImGui::IsItemHovered()/IsItemActive() gate the input to the panel that owns it): the input, then the
    // overlay (the orbit target, the speed HUD) drawn over the item's rectangle.
    void HandleCameraInput(const camera_rig& Rig) noexcept;
    void DrawCameraOverlay(const camera_rig& Rig) noexcept;

    class camera
    {
    public:
        xgpu::tools::view   m_View;
        xmath::radian3      m_Angles    = {};                     // zero: the default constructor leaves it as it was in memory (a NaN there asserted in LookAt, sometimes)
        float               m_Distance  = -1;                     // -1 until the first frame framed the subject
        xmath::fvec3        m_Target    = xmath::fvec3(0, 0, 0);
        bool                m_bReframe  = true;
        float               m_Radius    = 1.0f;                   // the subject: what Recenter/first-frame frames to, and what sizes the near/far depth range
        xmath::fvec3        m_Center    = xmath::fvec3(0, 0, 0);
        camera_state        m_State;

        // ORBIT: W A S D move the target this many distances per second (so flying feels the same whether zoomed in on a bolt or looking at a whole level)
        float               m_FlySpeed  = 1.0f;

        camera_rig          Rig(void) noexcept { return { m_State, m_View, m_Angles, m_Distance, m_Target, m_FlySpeed }; }

        // Reframes on the next UpdateView call (e.g. after loading a new subject, or the panel's own
        // "Recenter" button).
        void Recenter(void) noexcept { m_bReframe = true; }

        // See HandleCameraInput. Call right after the viewport item was submitted.
        void HandleInput(void) noexcept
        {
            HandleCameraInput(Rig());
            if (m_State.m_bFrame) { m_State.m_bFrame = false; m_bReframe = true; }       // F in FLY: frame the subject
        }

        // The target and the speed overlay: call it after the render callback of the viewport was added (it draws over the scene), still right after the viewport item
        void DrawOverlay(void) noexcept { DrawCameraOverlay(Rig()); }

        bool                m_bSubject  = true;                   // m_Center / m_Radius say what the view is about (false: the host does not set them)

        // Sizes the view to the panel, frames the subject when asked to, and places the camera. Min is
        // the panel's own ABSOLUTE on-screen rect (matching every editor's own prior UpdateView calls
        // byte for byte - RayFromScreen takes its own Viewport.Min into account internally).
        void UpdateView(const ImVec2& Min, float ViewW, float ViewH) noexcept;
    };
}

#endif // XEDITOR_TOOLS_CAMERA_H
