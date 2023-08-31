#version $GLSL_VERSION_STR
$GLSL_DEFAULT_PRECISION_FLOAT

#pragma vp_name MP GPU Lines Model
#pragma vp_entryPoint mp_MPLineDrawable_VS_MODEL
#pragma vp_location vertex_model
#pragma vp_order last

uniform mat4 osg_ViewMatrix;

// Ratio between the local sclae and the pixel scale
uniform float mp_local2pixel;

// Line width
uniform float mp_LineWidth;

// Input attributes for the side vector which is used to expand the line
in vec3 mp_MPLineDrawable_side;

// Input attributes for length of the line
in vec2 mp_MPLineDrawable_length;

// The interpolated length
out vec2 _length;

// Wether this vertex is at the opposite side of the globe
flat out int mp_MPLineDrawable_backFaceCulled;

// MissionPlus line pattern with one part plain fill and one part with alpha
uniform float oe_MPPatternThreshold; // percentage of plain line vs the whole line width

// Highlight management
uniform uint objectid_to_highlight;
uint oe_index_objectid;      // Stage global containing object id
flat out int selected;

// In model space, move the vertice along the side vector
void mp_MPLineDrawable_VS_MODEL(inout vec4 vertex)
{
    float widthByTwo = mp_LineWidth*0.5;

    // check if this vertex is at the opposite side of the globe
    vec4 earthCenter_view = osg_ViewMatrix * vec4(0., 0., 0., 1.);
    vec3 normal = (gl_ModelViewMatrix*vertex).xyz - earthCenter_view.xyz;
    mp_MPLineDrawable_backFaceCulled = normal.z < 0. ? 1 : 0;

    // compute the length of the side vector
    if (mp_MPLineDrawable_backFaceCulled == 0)
    {
        vertex.xyz -= mp_MPLineDrawable_side * (widthByTwo*mp_local2pixel);
        if (oe_MPPatternThreshold > 0.)
        {
            vertex.xyz -= mp_MPLineDrawable_length.y * mp_MPLineDrawable_side * ( (widthByTwo-oe_MPPatternThreshold*widthByTwo) * mp_local2pixel);
            _length.y = (1. + mp_MPLineDrawable_length.y) * widthByTwo;
        }
        else
        {
            _length.y = mp_MPLineDrawable_length.y * widthByTwo;
        }
    }

    // case culled -> so we don't expand the triangle in order to limit the rasterization
    else
    {
        _length.y = mp_MPLineDrawable_length.y * widthByTwo;
    }

    // Interpolate the length vector
    _length.x = mp_MPLineDrawable_length.x / mp_local2pixel;

    // check if this line is selected
    selected = (objectid_to_highlight > 1u && objectid_to_highlight == oe_index_objectid) ? 1 : 0;
}




[break]

#version $GLSL_VERSION_STR
$GLSL_DEFAULT_PRECISION_FLOAT

#pragma vp_name MP GPU Lines FS
#pragma vp_entryPoint mp_MPLineDrawable_FS
#pragma vp_location fragment_coloring

// Line width
uniform float mp_LineWidth;

// Stipple pattern
uniform int oe_GL_LineStippleFactor;
uniform int oe_GL_LineStipplePattern;

// MissionPlus line pattern with one part plain fill and one part with alpha
uniform float oe_MPPatternAlpha;
uniform float oe_MPPatternThreshold; // percentage of plain line vs the whole line width

in vec2 _length;

flat in int mp_MPLineDrawable_backFaceCulled;

// highlight management
flat in int selected;
uniform vec4 oe_LineDrawable_highlight_color;

void mp_MPLineDrawable_FS(inout vec4 color)
{
    // Dont draw if in the opposite side of the globe
    if (mp_MPLineDrawable_backFaceCulled == 1)
    {
        discard;
    }

    // Case the fragment is really visible
    else
    {
        // update color if highlighted
        if ( selected == 1 )
        {
            color = oe_LineDrawable_highlight_color;
        }

        // handle the stipple pattern
        if (oe_GL_LineStipplePattern != 0xffff)
        {
            // sample the stippling pattern (16-bits repeating)
            int ci = int(mod(_length.x, 16.0 * float(oe_GL_LineStippleFactor))) / oe_GL_LineStippleFactor;
            int pattern16 = 0xffff & (oe_GL_LineStipplePattern & (1 << ci));
            if (pattern16 == 0)
                color.a = 0.;
        }

        // handle the plain & transparent pattern
        if (oe_MPPatternThreshold > 0.)
        {
            float alpha = _length.y > 0. ? oe_MPPatternAlpha : 0.;
            float threshold = mp_LineWidth * oe_MPPatternThreshold;

            // anti-aliasing on inner separation between plain and transparent line
            color.a *= 1.-0.5*smoothstep( threshold - 0.125, threshold + 0.125, _length.y);

            // anti aliasing of the outer borders
            color.a *= smoothstep(0., 0.5, 0.5*mp_LineWidth - abs(_length.y-0.5*mp_LineWidth));
        }

        else
        {
            // anti-aliasing
            color.a *= smoothstep(0., 0.5, 0.5*mp_LineWidth - abs(_length.y));
        }
    }
}
