#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_ARB_gpu_shader_fp64 : enable

// Constants
const float GridScale = 1.0;

// Push constants
layout(set = 2, binding = 0) uniform Uniforms
{
    mat4    L2W;
    mat4    W2C;
    mat4    ShadowL2C;
    vec4    WorldSpaceCameraPos;
    float   MajorGridDiv;
    float   Pass;               // 0: the fill (pushed behind the ground plane), 1: the lines (pulled in front of it) - grid::Draw draws both
    float   FillAlpha;          // the fill's opacity: 0 the lines alone, what is below the grid shows through
    float   Rise;               // metres: the lines are depth-tested as the plane Rise above, the fill as Rise below (grid.cpp)
    vec2    JitterPx;           // the TAA jitter (pixels) the scene's depth was drawn with: the fragment shader matches its depth to it
    float   LineHalo;           // 0..1: the light outline around the dark lines (seen on surfaces of the lines' own colour)
} uniforms;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;   //[INPUT_COLOR]

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec3 outCameraPos;
layout(location = 2) out vec4 outShadowPos;

void main()
{
    vec4 worldPos4 = uniforms.L2W * vec4(inPosition, 1.0);
    vec3 worldPos = worldPos4.xyz;

    // Depth: the lines as the plane Rise metres ABOVE, the fill Rise BELOW, so a surface within Rise of the plane always shows the lines and
    // hides the fill (a floor at y 0, a foot on it). A scale about the eye: no pixel moves, the UVs stay exact, and the scaled plane is the
    // parallel plane at the eye's height times the scale. Ortho: along the view axis. (The fragment shader matches the scene's TAA jitter)
    const float Side = uniforms.Pass < 0.5 ? -1.0 : 1.0;
    vec3 depthPos = worldPos;
    {
        vec4 r3 = vec4(uniforms.W2C[0][3], uniforms.W2C[1][3], uniforms.W2C[2][3], uniforms.W2C[3][3]);
        const bool bOrtho = length(r3.xyz) < 0.0001 && abs(r3.w - 1.0) < 0.0001;
        const vec3 Eye    = uniforms.WorldSpaceCameraPos.xyz;
        if (bOrtho)
        {
            const vec3 F = normalize(vec3(uniforms.W2C[0][2], uniforms.W2C[1][2], uniforms.W2C[2][2]));
            const vec3 ToEye = -F * sign(dot(worldPos - Eye, F));       // (the view axis, toward the eye: its y is how much a step along it rises)
            depthPos += ToEye * (Side * uniforms.Rise / max(abs(ToEye.y), 0.05));
        }
        else
        {
            const float k = clamp(uniforms.Rise / max(Eye.y - worldPos.y, 1e-4), 0.0, 0.5);
            depthPos = Eye + (worldPos - Eye) * (1.0 - Side * k);
        }
    }
    gl_Position  = uniforms.W2C * vec4(depthPos, 1.0);
    outShadowPos = uniforms.ShadowL2C * vec4(inPosition, 1.0);

    // Assume WorldUV = true
    outUV = worldPos.xz * GridScale;

    // Detect if orthographic
    vec4 projRow3 = vec4(uniforms.W2C[0][3], uniforms.W2C[1][3], uniforms.W2C[2][3], uniforms.W2C[3][3]);
    float row3xyzLen = length(projRow3.xyz);
    bool isOrtho = (row3xyzLen < 0.0001) && (abs(projRow3.w - 1.0) < 0.0001);

    // Compute py = P[1][1]
    vec3 projRow1xyz = vec3(uniforms.W2C[0][1], uniforms.W2C[1][1], uniforms.W2C[2][1]);
    float py = length(projRow1xyz);
    if (py < 0.0001) py = 0.0001; // Avoid division by zero

    if (isOrtho) 
    {
        outCameraPos = vec3(0.0, 0.0, 1.0 / py);
    }
    else 
    {
        // Compute px = P[0][0]
        vec3 projRow0xyz = vec3(uniforms.W2C[0][0], uniforms.W2C[1][0], uniforms.W2C[2][0]);
        float px = length(projRow0xyz);
        if (px < 0.0001) px = 0.0001;

        // Compute pz length
        vec3 projRow2xyz = vec3(uniforms.W2C[0][2], uniforms.W2C[1][2], uniforms.W2C[2][2]);
        float pzLen = length(projRow2xyz);
        if (pzLen < 0.0001) pzLen = 0.0001;

        // Compute rotation matrix rows
        vec3 rotRow0 = projRow0xyz / px;
        vec3 rotRow1 = projRow1xyz / py;

        // Test positive pz
        vec3 tempRotRow2 = projRow2xyz / pzLen;
        vec3 crossProd = cross(rotRow1, tempRotRow2);
        float det = dot(rotRow0, crossProd);

        // Choose sign for left-handed (det = -1)
        float pzSign = (det < 0.0) ? 1.0 : -1.0;
        float pz = pzSign * pzLen;

        vec3 rotRow2 = projRow2xyz / pz;

        // Rotation matrix (rows)
        mat3 rot = mat3(rotRow0, rotRow1, rotRow2);

        // Compute view position
        dvec3 diff = dvec3(worldPos) - uniforms.WorldSpaceCameraPos.xyz;
        vec3 viewPos = rot * vec3(diff);

        // Adjust
        outCameraPos = viewPos / py;
    }
}


#if 0
#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec3 inPos;     //[INPUT_POSITION]
layout(location = 1) in vec2 inUV;      //[INPUT_UVS]
layout(location = 2) in vec4 inColor;   //[INPUT_COLOR]

layout(std140, push_constant) uniform Uniforms
{
    // Vertex shader
    mat4    L2W;
    mat4    W2C;
    vec3    WorldSpaceCameraPos;
    float   MajorGridDiv;

} uniforms;

#if defined(_AXIS_X)
    #define AXIS_COMPONENTS yz
#elif defined(_AXIS_Z)
    #define AXIS_COMPONENTS xy
#else
    #define AXIS_COMPONENTS xz
#endif

layout(location = 0) out struct 
{ 
    vec4 Color; 
    vec4 UV; 
} Out;

void main() 
{
    gl_Position = uniforms.W2C * (uniforms.L2W * vec4(inPos, 1.0));
    float   div                     = max(2.0, floor(uniforms.MajorGridDiv + 0.5));
    vec3    worldPos                = (uniforms.L2W * vec4(inPos.xyz, 1.0)).xyz;
    vec3    cameraCenteringOffset   = floor(uniforms.WorldSpaceCameraPos / div) * div;

    Out.UV.yx = (worldPos - cameraCenteringOffset).AXIS_COMPONENTS;
    Out.UV.wz = worldPos.AXIS_COMPONENTS;
    Out.Color = inColor;
}
#endif
