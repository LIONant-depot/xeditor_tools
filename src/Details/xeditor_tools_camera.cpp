#include "../xeditor_tools_camera.h"
#include "dependencies/imgui/imgui.h"
#include "dependencies/xGPU/source/Tools/editors/xgpu_editor_viewport.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

// See xeditor_tools_camera.h for what the two modes do. Both work on the same numbers (angles, distance, target): the position of the eye is always
// Target + RotateY(yaw)(RotateX(pitch)((0, 0, distance))), so
//   - ORBIT turns the angles and keeps the target: the eye goes around it;
//   - FLY turns the angles and moves the target so that the eye stays where it was: the camera turns where it is.
// Moving (W A S D Q E, the middle drag) shifts the target by a vector in both: the whole rig moves with it, which is what "fly forward" means here.
// The keys and the right drag need the right mouse button (Unity's convention): a host's own keys (the Level editor's Q W E R tools) keep working otherwise.
namespace xeditor_tools
{
    namespace
    {
        constexpr float k_MinSpeed = 0.05f, k_MaxSpeed = 2000.0f;           // meters per second

        xmath::fvec3 EyeOffset(const xmath::radian3& Angles, float Distance) noexcept
        {
            xmath::fvec3 Offset(0, 0, Distance);
            Offset.RotateX(Angles.m_Pitch);
            Offset.RotateY(Angles.m_Yaw);
            return Offset;
        }

        // What a speed feels like
        const char* SpeedWord(float Speed) noexcept
        {
            if (Speed < 0.5f)   return "Crawling";
            if (Speed < 2.0f)   return "Strolling";
            if (Speed < 8.0f)   return "Jogging";
            if (Speed < 25.0f)  return "Sprinting";
            if (Speed < 80.0f)  return "Driving";
            if (Speed < 300.0f) return "Flying a plane";
            if (Speed < 1000.0f)return "Supersonic";
            return "Ludicrous speed";
        }

        // 0..1 on a log scale from the slowest to the fastest speed
        float SpeedPosition(float Speed) noexcept
        {
            return std::clamp(std::log(Speed / k_MinSpeed) / std::log(k_MaxSpeed / k_MinSpeed), 0.0f, 1.0f);
        }

        ImU32 Alpha(ImU32 Color, float A) noexcept
        {
            const float Own = static_cast<float>((Color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            return (Color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(Own * A, 0.0f, 1.0f) * 255.0f) << IM_COL32_A_SHIFT);
        }

        // blue (slow) -> green -> yellow -> red (fast)
        ImU32 SpeedColor(float t) noexcept
        {
            const ImVec4 Stops[] = { ImVec4(0.25f, 0.55f, 1.0f, 1), ImVec4(0.2f, 0.9f, 0.5f, 1), ImVec4(1.0f, 0.85f, 0.2f, 1), ImVec4(1.0f, 0.3f, 0.2f, 1) };
            const float x = std::clamp(t, 0.0f, 1.0f) * 3.0f;
            const int   i = std::min(2, static_cast<int>(x));
            const float f = x - static_cast<float>(i);
            const ImVec4 C(Stops[i].x + (Stops[i + 1].x - Stops[i].x) * f, Stops[i].y + (Stops[i + 1].y - Stops[i].y) * f, Stops[i].z + (Stops[i + 1].z - Stops[i].z) * f, 1.0f);
            return ImGui::ColorConvertFloat4ToU32(C);
        }

        bool Project(const camera_rig& Rig, const xmath::fvec3& P, const ImVec2& Min, const ImVec2& Size, ImVec2& Out) noexcept
        {
            // NOT "1 - NDC.y": the projection already flips Y for this engine's Vulkan convention (see xskeleton's RenderLabels)
            const xmath::fvec4 Clip = Rig.m_View.getW2C() * xmath::fvec4(P, 1.0f);
            if (Clip.m_W <= 1.0e-4f) return false;
            Out = ImVec2(Min.x + (Clip.m_X / Clip.m_W * 0.5f + 0.5f) * Size.x, Min.y + (Clip.m_Y / Clip.m_W * 0.5f + 0.5f) * Size.y);
            return true;
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------------------------

    void HandleCameraInput(const camera_rig& Rig) noexcept
    {
        // A wide lens stretches what is away from the middle: the user chooses. Flying, the eye backs away (or comes closer) along the view while the lens changes, so
        // what the camera looks at keeps its size on the screen (the dolly zoom): the distance follows tan(fov / 2).
        {
            auto& St = Rig.m_State;
            if (St.m_LastFovDeg > 0.0f && St.m_LastFovDeg != St.m_FovDeg && Rig.m_Distance > 0.0f)
            {
                const float Ratio = std::tan(St.m_LastFovDeg * 0.0087266462599716f) / std::tan(St.m_FovDeg * 0.0087266462599716f);
                if (St.m_Mode == camera_mode::FLY && St.m_bSubject)
                {
                    // The depth of the subject (along the view) is scaled by the ratio: the eye moves along the view, the target with it
                    const xmath::fvec3 Eye     = Rig.m_Target + EyeOffset(Rig.m_Angles, Rig.m_Distance);
                    const xmath::fvec3 Forward = (Rig.m_Target - Eye).NormalizeSafeCopy();
                    const float        Depth   = (St.m_Subject - Eye).Dot(Forward);
                    if (Depth > 1.0e-4f)
                    {
                        const xmath::fvec3 NewEye = Eye + Forward * (Depth - Depth * Ratio);
                        Rig.m_Target = NewEye + Forward * Rig.m_Distance;
                    }
                }
                else Rig.m_Distance *= Ratio;
            }
            St.m_LastFovDeg = St.m_FovDeg;
            Rig.m_View.setFov(xmath::radian{ St.m_FovDeg * 0.017453292519943f });
        }
        auto&        io   = ImGui::GetIO();
        auto&        S    = Rig.m_State;
        const double Now  = ImGui::GetTime();

        // A little momentum (Unreal's): the rig speeds up toward the speed the keys ask for and slows down when they let go, and the wheel's zoom glides. Runs every frame,
        // also when the mouse is elsewhere, so the camera does not stop dead.
        xmath::fvec3 Desired(0, 0, 0);
        auto Glide = [&]
        {
            const float Dt = std::min(io.DeltaTime, 0.1f);
            const float Tau = Desired.Length() > 0.0f ? 0.28f : 0.12f;        // it takes its time to pick up speed, and stops a little faster
            S.m_Velocity += (Desired - S.m_Velocity) * (1.0f - std::exp(-Dt / Tau));
            if (S.m_Velocity.Length() > 1.0e-4f * std::max(S.m_Speed, 0.1f)) { Rig.m_Target += S.m_Velocity * Dt; S.m_Active = Now; }
            else                                                              S.m_Velocity = xmath::fvec3(0, 0, 0);

            // The turn and the slide of the mouse: the drag is a low pass (a few frames), so the view eases in and keeps going a moment after the mouse stops
            if (std::fabs(S.m_TurnPitchLeft) > 1.0e-5f || std::fabs(S.m_TurnYawLeft) > 1.0e-5f)
            {
                const float Ease = 1.0f - std::exp(-Dt / 0.06f);
                const xmath::fvec3 Eye = Rig.m_Target + EyeOffset(Rig.m_Angles, std::max(Rig.m_Distance, 0.0f));
                const float P = S.m_TurnPitchLeft * Ease, Y = S.m_TurnYawLeft * Ease;
                S.m_TurnPitchLeft -= P; S.m_TurnYawLeft -= Y;
                Rig.m_Angles.m_Pitch.m_Value += P;
                Rig.m_Angles.m_Yaw.m_Value   += Y;
                if (S.m_Mode == camera_mode::FLY && Rig.m_Distance > 0.0f) Rig.m_Target = Eye - EyeOffset(Rig.m_Angles, Rig.m_Distance);      // the eye stays where it was
                S.m_Active = Now;
            }
            else { S.m_TurnPitchLeft = 0.0f; S.m_TurnYawLeft = 0.0f; }
            if (S.m_SlideLeft.Length() > 1.0e-6f)
            {
                const xmath::fvec3 Step = S.m_SlideLeft * (1.0f - std::exp(-Dt / 0.06f));
                S.m_SlideLeft -= Step;
                Rig.m_Target  += Step;
                S.m_Active     = Now;
            }
            else S.m_SlideLeft = xmath::fvec3(0, 0, 0);

            if (std::fabs(S.m_ZoomLeft) > 1.0e-4f && Rig.m_Distance > 0.0f)
            {
                const float Step = S.m_ZoomLeft * (1.0f - std::exp(-Dt / 0.07f));
                S.m_ZoomLeft    -= Step;
                Rig.m_Distance  *= std::exp(Step);
                // Closer than the minimum the camera pushes its target forward (it goes through): the minimum is of the size of the subject, not a meter
                const float Min = S.m_SubjectRadius > 0.0f ? std::max(S.m_SubjectRadius * 0.1f, 1.0e-4f) : 0.5f;
                if (Rig.m_Distance < Min)
                {
                    Rig.m_Target  += Rig.m_View.getWorldZVector() * (Min * (Min - Rig.m_Distance));
                    Rig.m_Distance = Min;
                    S.m_ZoomLeft   = 0.0f;
                }
                S.m_Active = Now;
            }
        };
        if (!ImGui::IsItemHovered() && !ImGui::IsItemActive()) { Glide(); return; }
        const bool   bRMB = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        const bool   bMMB = ImGui::IsMouseDown(ImGuiMouseButton_Middle);

        // F switches the mode (not while typing, and not as part of a chord)
        if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            S.SwitchMode(Now);
        }

        const bool bFly = S.m_Mode == camera_mode::FLY;

        // Turn: the orbit moves the eye around the target; the fly turns where it is (the target moves so that the eye does not)
        if (bRMB && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
        {
            const float Turn = bFly ? 0.0028f : 0.01f;          // radians per pixel: the fly camera turns where it is, the whole view swings, so it is gentler
            S.m_TurnPitchLeft -= Turn * io.MouseDelta.y;        // (Glide turns it, a little at a time)
            S.m_TurnYawLeft   -= Turn * io.MouseDelta.x;
            S.m_Active = Now;
        }

        // Slide: the fly one at the fly speed (a pixel is m_Speed / 100 meters), the orbit one a fixed step as it always was
        if (bMMB && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
        {
            // Orbit: the point under the mouse follows it (a pixel is the world size of a pixel at the target's depth); Fly: at the fly speed
            const float Height = std::max(ImGui::GetItemRectSize().y, 1.0f);
            const float Step   = bFly ? S.m_Speed * 0.002f : 2.0f * std::max(Rig.m_Distance, 0.0f) * std::tan(S.m_FovDeg * 0.0087266462599716f) / Height;
            S.m_SlideLeft += Rig.m_View.getWorldYVector() * (Step * io.MouseDelta.y);
            S.m_SlideLeft += Rig.m_View.getWorldXVector() * (Step * io.MouseDelta.x);
            S.m_Active = Now;
        }

        // The wheel: the distance to the target (orbit) or the speed (fly)
        if (io.MouseWheel != 0.0f)
        {
            if (bFly)
            {
                S.m_SpeedFrom    = S.m_Speed;
                S.m_Speed        = std::clamp(S.m_Speed * std::pow(1.2f, io.MouseWheel), k_MinSpeed, k_MaxSpeed);
                S.m_SpeedChanged = Now;
            }
            else if (Rig.m_Distance != -1)
            {
                S.m_ZoomDir  = io.MouseWheel > 0.0f ? 1.0f : -1.0f;
                S.m_ZoomTime = Now;
                S.m_ZoomLeft += -0.2f * io.MouseWheel;          // done by Glide, over a few frames
                S.m_Active    = Now;
            }
        }

        // Move: right mouse held (see the top comment) and not typing
        if (bRMB && !io.WantTextInput)
        {
            xmath::fvec3 Move(0, 0, 0);
            if (ImGui::IsKeyDown(ImGuiKey_W)) Move += Rig.m_View.getWorldZVector();
            if (ImGui::IsKeyDown(ImGuiKey_S)) Move -= Rig.m_View.getWorldZVector();
            if (ImGui::IsKeyDown(ImGuiKey_A)) Move += Rig.m_View.getWorldXVector();
            if (ImGui::IsKeyDown(ImGuiKey_D)) Move -= Rig.m_View.getWorldXVector();
            if (ImGui::IsKeyDown(ImGuiKey_E)) Move += Rig.m_View.getWorldYVector();
            if (ImGui::IsKeyDown(ImGuiKey_Q)) Move -= Rig.m_View.getWorldYVector();
            if (Move.Length() > 0.0f)
            {
                const float Speed = bFly ? S.m_Speed * (io.KeyShift ? 3.0f : 1.0f)                                   // meters per second
                                         : Rig.m_OrbitFlySpeed * std::max(Rig.m_Distance, 0.1f);                     // distances per second
                Desired = Move.Normalize() * Speed;
            }
        }
        Glide();
    }

    //----------------------------------------------------------------------------------------------------------------------------------------------------

    void DrawCameraOverlay(const camera_rig& Rig) noexcept
    {
        const ImVec2 Min  = ImGui::GetItemRectMin();
        const ImVec2 Max  = ImGui::GetItemRectMax();
        const ImVec2 Size(Max.x - Min.x, Max.y - Min.y);
        if (Size.x < 80.0f || Size.y < 60.0f) return;

        // The host sets the viewport of its view while it renders (a callback that runs after this overlay): the first frame of a panel, or one that
        // was not rendered yet, has none and getViewport asserts. The overlay projects with a copy that has its own rectangle: the host's view is
        // not touched (the click-pick of the same frame builds its ray from it, in screen coordinates)
        xgpu::tools::view ProjView = Rig.m_View;
        ProjView.setViewport({ 0, 0, static_cast<int>(Size.x), static_cast<int>(Size.y) });
        const camera_rig ProjRig{ Rig.m_State, ProjView, Rig.m_Angles, Rig.m_Distance, Rig.m_Target };

        auto&        S    = Rig.m_State;
        const double Now  = ImGui::GetTime();
        auto*        pDraw = ImGui::GetWindowDrawList();
        const bool   bFly = S.m_Mode == camera_mode::FLY;
        pDraw->PushClipRect(Min, Max, true);

        //
        // ORBIT: the target. Three rings, one per world axis (they turn with the camera, so they read as a little globe), a dot in the middle and four ticks.
        // Quiet while nothing happens, bright and pulsing while the user turns, moves or zooms.
        //
        if (!bFly && Rig.m_Distance > 0.0f)
        {
            ImVec2 Center;
            if (Project(ProjRig, Rig.m_Target, Min, Size, Center))
            {
                // The globe has a size in the WORLD, a round one (1, 2, 5 x 10^n meters): the biggest that fits in 64 pixels. Going closer makes it grow, until
                // the next round size takes over - like the scale bar of a map. The distance to the target is written under it.
                float WorldRadius = 0.0f, Radius = 0.0f;
                for (int Exp = 3; Exp >= -5 && WorldRadius == 0.0f; --Exp)
                    for (float Mantissa : { 5.0f, 2.0f, 1.0f })
                    {
                        const float R = Mantissa * std::pow(10.0f, static_cast<float>(Exp));
                        ImVec2 Edge;
                        if (!Project(ProjRig, Rig.m_Target + Rig.m_View.getWorldXVector() * R, Min, Size, Edge)) continue;
                        const float Px = std::sqrt((Edge.x - Center.x) * (Edge.x - Center.x) + (Edge.y - Center.y) * (Edge.y - Center.y));
                        if (Px <= 64.0f && Px >= 10.0f) { WorldRadius = R; Radius = Px; break; }
                    }
                if (WorldRadius == 0.0f) { WorldRadius = Rig.m_Distance * 0.06f; Radius = 24.0f; }

                const float Heat   = std::clamp(1.0f - static_cast<float>(Now - S.m_Active) / 0.9f, 0.0f, 1.0f);     // 1 while it is being used, fading to 0
                const float A      = 0.5f + 0.5f * Heat;

                // The glow and the pulse that goes out of the target while it is used
                pDraw->AddCircleFilled(Center, Radius * 1.4f, Alpha(IM_COL32(255, 255, 255, 18), A), 32);
                if (Heat > 0.0f)
                {
                    const float Wave = std::fmod(static_cast<float>(Now) * 1.6f, 1.0f);
                    pDraw->AddCircle(Center, Radius * (1.0f + Wave * 0.6f), Alpha(IM_COL32(255, 255, 255, 110), Heat * (1.0f - Wave)), 40, 1.5f);
                }

                // The rings (world axes), 24 points each
                const ImU32  Colors[3] = { IM_COL32(255, 90, 90, 255), IM_COL32(110, 230, 110, 255), IM_COL32(100, 150, 255, 255) };
                constexpr int N = 24;
                for (int Axis = 0; Axis < 3; ++Axis)
                {
                    ImVec2 Points[N];
                    int    Count = 0;
                    for (int i = 0; i < N; ++i)
                    {
                        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(N), c = std::cos(a) * WorldRadius, s = std::sin(a) * WorldRadius;
                        const xmath::fvec3 P = Axis == 0 ? Rig.m_Target + xmath::fvec3(0, c, s) : Axis == 1 ? Rig.m_Target + xmath::fvec3(c, 0, s) : Rig.m_Target + xmath::fvec3(c, s, 0);
                        if (Project(ProjRig, P, Min, Size, Points[Count])) ++Count;
                    }
                    if (Count == N) pDraw->AddPolyline(Points, N, Alpha(Colors[Axis], 0.85f * A), ImDrawFlags_Closed, 2.0f);
                }

                // The heart and the ticks
                const float Tick = Radius * 0.35f;
                for (int i = 0; i < 4; ++i)
                {
                    const ImVec2 D(i == 0 ? 1.0f : i == 1 ? -1.0f : 0.0f, i == 2 ? 1.0f : i == 3 ? -1.0f : 0.0f);
                    pDraw->AddLine(ImVec2(Center.x + D.x * Radius * 1.12f, Center.y + D.y * Radius * 1.12f), ImVec2(Center.x + D.x * (Radius * 1.12f + Tick), Center.y + D.y * (Radius * 1.12f + Tick)), Alpha(IM_COL32(255, 255, 255, 220), A), 1.4f);
                }
                pDraw->AddCircleFilled(Center, 3.2f, Alpha(IM_COL32(0, 0, 0, 200), A), 12);
                pDraw->AddCircleFilled(Center, 2.2f, Alpha(IM_COL32(255, 255, 255, 255), A), 12);

                // Chevrons that run toward the target (closer) or away from it (farther) for a moment after the wheel
                if (const float z = static_cast<float>(Now - S.m_ZoomTime); z >= 0.0f && z < 0.7f && S.m_ZoomDir != 0.0f)
                {
                    const float Fade = 1.0f - z / 0.7f;
                    for (int i = 0; i < 4; ++i)
                    {
                        const ImVec2 D(i == 0 ? 1.0f : i == 1 ? -1.0f : 0.0f, i == 2 ? 1.0f : i == 3 ? -1.0f : 0.0f);
                        const ImVec2 Perp(-D.y, D.x);
                        const float  Phase = std::fmod(static_cast<float>(Now) * 3.0f, 1.0f);
                        const float  Dist  = Radius * (1.5f + 1.2f * (S.m_ZoomDir > 0.0f ? 1.0f - Phase : Phase));      // inward when closer, outward when farther
                        const ImVec2 Tip(Center.x + D.x * Dist, Center.y + D.y * Dist);
                        const float  Back  = S.m_ZoomDir > 0.0f ? 8.0f : -8.0f;                                          // the point faces where it goes
                        const ImVec2 Tail(Tip.x + D.x * Back, Tip.y + D.y * Back);
                        const ImU32  C = Alpha(IM_COL32(255, 255, 255, 255), Fade * 0.9f);
                        pDraw->AddLine(Tip, ImVec2(Tail.x + Perp.x * 6.0f, Tail.y + Perp.y * 6.0f), C, 2.0f);
                        pDraw->AddLine(Tip, ImVec2(Tail.x - Perp.x * 6.0f, Tail.y - Perp.y * 6.0f), C, 2.0f);
                    }
                }

                // The distance, and what the globe measures
                auto Meters = [](float V, char* Out, size_t Len)
                {
                    if      (V >= 1000.0f) std::snprintf(Out, Len, "%.3g km", V / 1000.0f);
                    else if (V >= 1.0f)    std::snprintf(Out, Len, "%.3g m",  V);
                    else if (V >= 0.01f)   std::snprintf(Out, Len, "%.3g cm", V * 100.0f);
                    else                   std::snprintf(Out, Len, "%.3g mm", V * 1000.0f);
                };
                char Dist[32], Ring[32], Text[80];
                Meters(Rig.m_Distance, Dist, sizeof(Dist));
                Meters(WorldRadius, Ring, sizeof(Ring));
                std::snprintf(Text, sizeof(Text), "%s away   (ring %s)", Dist, Ring);
                const ImVec2 TS = ImGui::CalcTextSize(Text);
                const ImVec2 At(Center.x - TS.x * 0.5f, Center.y + Radius * 1.12f + Tick + 6.0f);
                pDraw->AddRectFilled(ImVec2(At.x - 5.0f, At.y - 2.0f), ImVec2(At.x + TS.x + 5.0f, At.y + TS.y + 2.0f), Alpha(IM_COL32(10, 10, 14, 150), A), 5.0f);
                pDraw->AddText(At, Alpha(IM_COL32(255, 255, 255, 255), A), Text);
            }
        }

        if (const float t = static_cast<float>(Now - S.m_ModeChanged); t >= 0.0f && t < 1.4f)
        {
            const float  A    = std::clamp(1.4f - t, 0.0f, 0.5f) * 2.0f;
            const char*  Word = bFly ? "FLY" : "ORBIT";
            const char*  Sub  = bFly ? "turn where you stand | W A S D Q E fly (right mouse held) | wheel: speed" : "turn around the target | wheel changes the distance";
            const float  Big  = ImGui::GetFontSize() * 2.4f;
            const ImVec2 WS   = ImGui::GetFont()->CalcTextSizeA(Big, FLT_MAX, 0.0f, Word);
            const ImVec2 SS   = ImGui::CalcTextSize(Sub);
            const ImVec2 C(Min.x + Size.x * 0.5f, Min.y + Size.y * 0.22f);
            const float  W    = std::max(WS.x, SS.x) + 40.0f, H = WS.y + SS.y + 26.0f;
            pDraw->AddRectFilled(ImVec2(C.x - W * 0.5f, C.y - 10.0f), ImVec2(C.x + W * 0.5f, C.y - 10.0f + H), Alpha(IM_COL32(15, 15, 20, 190), A), 10.0f);
            pDraw->AddRect(ImVec2(C.x - W * 0.5f, C.y - 10.0f), ImVec2(C.x + W * 0.5f, C.y - 10.0f + H), Alpha(bFly ? IM_COL32(255, 200, 80, 255) : IM_COL32(110, 200, 255, 255), A), 10.0f, 0, 2.0f);
            pDraw->AddText(ImGui::GetFont(), Big, ImVec2(C.x - WS.x * 0.5f, C.y), Alpha(bFly ? IM_COL32(255, 200, 80, 255) : IM_COL32(110, 200, 255, 255), A), Word);
            pDraw->AddText(ImVec2(C.x - SS.x * 0.5f, C.y + WS.y + 6.0f), Alpha(IM_COL32(230, 230, 230, 255), A), Sub);
        }

        //
        // FLY: while the wheel changes the speed, a gauge on a log scale (0.05 to 2000 m/s) with the speed in big numbers, what it feels like, and how much
        // this step changed it
        //
        if (bFly)
        {
            const float t = static_cast<float>(Now - S.m_SpeedChanged);
            if (t >= 0.0f && t < 2.0f)
            {
                const float A = std::clamp(2.0f - t, 0.0f, 0.6f) / 0.6f;
                const float Pos = SpeedPosition(S.m_Speed), From = SpeedPosition(S.m_SpeedFrom);
                const ImU32 Hot = SpeedColor(Pos);

                const float  W = std::min(Size.x - 40.0f, 420.0f), H = 112.0f;
                const ImVec2 P0(Min.x + (Size.x - W) * 0.5f, Min.y + 14.0f), P1(P0.x + W, P0.y + H);
                pDraw->AddRectFilled(P0, P1, Alpha(IM_COL32(14, 14, 20, 205), A), 12.0f);
                pDraw->AddRect(P0, P1, Alpha(Hot, 0.9f * A), 12.0f, 0, 2.0f);

                // The speed, big, and what it feels like
                char Number[32];
                std::snprintf(Number, sizeof(Number), S.m_Speed < 10.0f ? "%.2f m/s" : S.m_Speed < 100.0f ? "%.1f m/s" : "%.0f m/s", S.m_Speed);
                const float  Big = ImGui::GetFontSize() * 2.2f;
                const ImVec2 NS  = ImGui::GetFont()->CalcTextSizeA(Big, FLT_MAX, 0.0f, Number);
                pDraw->AddText(ImGui::GetFont(), Big, ImVec2(P0.x + 18.0f, P0.y + 10.0f), Alpha(IM_COL32(255, 255, 255, 255), A), Number);
                pDraw->AddText(ImVec2(P0.x + 18.0f, P0.y + 14.0f + NS.y), Alpha(Hot, A), SpeedWord(S.m_Speed));

                // The step: an arrow and the percentage
                const float Change = (S.m_Speed / std::max(S.m_SpeedFrom, 1.0e-4f) - 1.0f) * 100.0f;
                if (std::fabs(Change) > 0.5f)
                {
                    const bool   bUp = Change > 0.0f;
                    const ImVec2 AC(P1.x - 70.0f, P0.y + 36.0f);
                    const ImU32  AColor = Alpha(bUp ? IM_COL32(255, 140, 60, 255) : IM_COL32(90, 190, 255, 255), A);
                    if (bUp) pDraw->AddTriangleFilled(ImVec2(AC.x, AC.y - 11.0f), ImVec2(AC.x - 10.0f, AC.y + 7.0f), ImVec2(AC.x + 10.0f, AC.y + 7.0f), AColor);
                    else     pDraw->AddTriangleFilled(ImVec2(AC.x, AC.y + 11.0f), ImVec2(AC.x - 10.0f, AC.y - 7.0f), ImVec2(AC.x + 10.0f, AC.y - 7.0f), AColor);
                    char Delta[24];
                    std::snprintf(Delta, sizeof(Delta), "%+.0f%%", Change);
                    pDraw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.3f, ImVec2(AC.x + 16.0f, AC.y - 9.0f), AColor, Delta);
                }

                // The gauge: a track in the colours of the speeds, filled up to the speed, a marker where it is, a ghost where it was, a tick for each decade
                const float GX0 = P0.x + 18.0f, GX1 = P1.x - 18.0f, GY = P1.y - 24.0f;
                const int   Segments = 48;
                for (int i = 0; i < Segments; ++i)
                {
                    const float a = static_cast<float>(i) / Segments, b = static_cast<float>(i + 1) / Segments;
                    const bool  bFilled = b <= Pos + 1.0f / Segments;
                    pDraw->AddRectFilled(ImVec2(GX0 + (GX1 - GX0) * a, GY - 4.0f), ImVec2(GX0 + (GX1 - GX0) * b + 0.5f, GY + 4.0f), Alpha(SpeedColor((a + b) * 0.5f), (bFilled ? 1.0f : 0.22f) * A));
                }
                const float Decades[] = { 0.1f, 1.0f, 10.0f, 100.0f, 1000.0f };
                for (float D : Decades)
                {
                    const float x = GX0 + (GX1 - GX0) * SpeedPosition(D);
                    char Label[16];
                    std::snprintf(Label, sizeof(Label), "%g", D);
                    pDraw->AddLine(ImVec2(x, GY + 6.0f), ImVec2(x, GY + 11.0f), Alpha(IM_COL32(220, 220, 220, 200), A), 1.0f);
                    const ImVec2 LS = ImGui::CalcTextSize(Label);
                    pDraw->AddText(ImVec2(x - LS.x * 0.5f, GY + 11.0f), Alpha(IM_COL32(200, 200, 200, 190), A), Label);
                }
                const float GhostX = GX0 + (GX1 - GX0) * From, MarkX = GX0 + (GX1 - GX0) * Pos;
                pDraw->AddLine(ImVec2(GhostX, GY - 9.0f), ImVec2(GhostX, GY + 9.0f), Alpha(IM_COL32(255, 255, 255, 120), A), 2.0f);
                pDraw->AddTriangleFilled(ImVec2(MarkX, GY - 5.0f), ImVec2(MarkX - 7.0f, GY - 15.0f), ImVec2(MarkX + 7.0f, GY - 15.0f), Alpha(IM_COL32(255, 255, 255, 255), A));
                pDraw->AddCircle(ImVec2(MarkX, GY), 7.0f, Alpha(IM_COL32(255, 255, 255, 255), A), 16, 2.0f);
            }
        }

        pDraw->PopClipRect();
    }

    //----------------------------------------------------------------------------------------------------------------------------------------------------

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
        m_State.m_bSubject = m_bSubject; m_State.m_Subject = m_Center; m_State.m_SubjectRadius = m_Radius;
    }
}
