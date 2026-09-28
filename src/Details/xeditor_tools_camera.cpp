#include "../xeditor_tools_camera.h"
#include "dependencies/imgui/imgui.h"
#include "dependencies/xGPU/source/tools/editors/xgpu_editor_viewport.h"

// WASD/QE fly is gated behind the right mouse button, Unity's Scene-view convention (direct user
// request - a prior version of this file deliberately left it ungated, but that meant W also always
// doubled as whatever else a host editor bound to the W key, e.g. the Level Editor's own Move-tool
// hotkey, firing both at once on every keypress). It is an EXTENSION of the existing orbit camera, not
// a replacement or a second mode to switch into: WASD/QE shift m_Target by a world-space vector,
// exactly the same trick middle-mouse-drag already uses for panning (see below) - Distance and Angles
// never change, so rotate/pan/zoom keep behaving exactly as they did before flying anywhere. The
// camera's position is always Target + sphericalOffset(Distance, Angles); shifting Target by a delta
// moves that whole implied rig by the same delta, which is exactly what "fly forward" means here.
namespace xeditor_tools
{
    void camera::HandleInput(void) noexcept
    {
        if (!ImGui::IsItemHovered() && !ImGui::IsItemActive()) return;
        auto& io = ImGui::GetIO();

        if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            m_Angles.m_Pitch.m_Value -= 0.01f * io.MouseDelta.y;
            m_Angles.m_Yaw.m_Value   -= 0.01f * io.MouseDelta.x;
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            m_Target += m_View.getWorldYVector() * (0.005f * io.MouseDelta.y);
            m_Target += m_View.getWorldXVector() * (0.005f * io.MouseDelta.x);
        }
        if (m_Distance != -1)
        {
            m_Distance += m_Distance * -0.2f * io.MouseWheel;
            if (m_Distance < 0.5f)
            {
                m_Target += m_View.getWorldZVector() * (0.5f * (0.5f - m_Distance));
                m_Distance = 0.5f;
            }
        }

        // Right mouse button held = flying (Unity convention, see this file's top comment), plus the
        // usual "don't steal keys from a text box" WantTextInput check.
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && !io.WantTextInput)
        {
            const float Speed = m_FlySpeed * std::max(m_Distance, 0.1f) * io.DeltaTime;
            xmath::fvec3 Move(0, 0, 0);
            if (ImGui::IsKeyDown(ImGuiKey_W)) Move += m_View.getWorldZVector();
            if (ImGui::IsKeyDown(ImGuiKey_S)) Move -= m_View.getWorldZVector();
            if (ImGui::IsKeyDown(ImGuiKey_A)) Move += m_View.getWorldXVector();
            if (ImGui::IsKeyDown(ImGuiKey_D)) Move -= m_View.getWorldXVector();
            if (ImGui::IsKeyDown(ImGuiKey_E)) Move += m_View.getWorldYVector();
            if (ImGui::IsKeyDown(ImGuiKey_Q)) Move -= m_View.getWorldYVector();
            if (Move.Length() > 0.0f) m_Target += Move.Normalize() * Speed;
        }
    }

    void camera::UpdateView(const ImVec2& Min, float ViewW, float ViewH) noexcept
    {
        m_View.setViewport({ static_cast<int>(Min.x), static_cast<int>(Min.y), static_cast<int>(Min.x + ViewW), static_cast<int>(Min.y + ViewH) });
        // View defaults to aspect 1 (square). FOV projection uses ScaleX = -Focal/aspect, so a
        // non-square panel must get the real W/H here or the projected scene (and any screen math that
        // shares it, e.g. RayFromScreen) drifts as the dock is resized.
        m_View.setAspect(ViewW / ViewH);
        if (m_bReframe)
        {
            m_bReframe = false;
            xgpu::tools::editors::ReframeOrbitCamera(m_View, m_Radius, m_Center, m_Distance, m_Target);
        }
        m_View.LookAt(m_Distance, m_Angles, m_Target);
    }
}
