#version $GLSL_VERSION_STR
$GLSL_DEFAULT_PRECISION_FLOAT

#pragma vp_entryPoint oe_anno_VS
#pragma vp_location   vertex_model

#pragma import_defines(TYPE_CHARACTER_MSDF, TYPE_BBOX_NO_PICK, TYPE_BBOX_STROKE_SIDED, TYPE_ICON)


in vec4 oe_anno_attr_info;
in vec4 oe_anno_attr_color2;

in vec3 oe_anno_attr_circle_center;
in vec3 oe_anno_attr_circle_anchor;

out vec2 oe_anno_texcoord;

flat out vec4 oe_anno_info;
flat out vec4 oe_anno_color2;
flat out float oe_anno_stroke_width;
flat out float oe_anno_fill_white_threshold;
flat out float widthBy2;
flat out float heightBy2;
flat out float msdfUnit;

// ----------- Text on circle -------------

// ratio between the local scale and the pixel scale
uniform float mp_local2pixel;

// --------- Highlight management ---------
uniform uint objectid_to_highlight;
uniform float oe_anno_highlightStrokeWidth;
uniform vec4 oe_anno_highlightStrokeColor;
// Stage global containing object id
uint oe_index_objectid;
flat out int selected;

mat3 rotAxis(in vec3 axis, in float a) {
    axis = normalize(axis);
    float s = sin(a);
    float c = cos(a);
    float oc = 1. - c;
    vec3 as = axis * s;
    mat3 p = mat3(axis.x*axis, axis.y*axis, axis.z*axis);
    mat3 q = mat3(c, -as.z, as.y, as.z, c, -as.x, -as.y, as.x, c);
    return p*oc + q;
}

void oe_anno_VS(inout vec4 vertex)
{
    oe_anno_texcoord = gl_MultiTexCoord0.st;
    oe_anno_info = oe_anno_attr_info;
    // is this item selected ?
    selected = (objectid_to_highlight > 1u && objectid_to_highlight == oe_index_objectid) ? 1 : 0;

    if ( oe_anno_info.z != TYPE_CHARACTER_MSDF && oe_anno_info.z != TYPE_ICON )
    {
        oe_anno_fill_white_threshold = floor(oe_anno_info.w);
        oe_anno_stroke_width = (oe_anno_info.w - oe_anno_fill_white_threshold) * 10.;

        if ( selected == 1 && oe_anno_info.z != TYPE_BBOX_STROKE_SIDED && oe_anno_info.z != TYPE_BBOX_NO_PICK )
        {
            float deltaStroke = oe_anno_highlightStrokeWidth - oe_anno_stroke_width;
            oe_anno_stroke_width = oe_anno_highlightStrokeWidth;
            oe_anno_color2 = oe_anno_highlightStrokeColor;
            if (vertex.x < 0.)
                vertex.x -= deltaStroke;
            else
                vertex.x += deltaStroke;

            if (vertex.y < 0.)
                vertex.y -= deltaStroke;
            else
                vertex.y += deltaStroke;
        }
        else
        {
            oe_anno_color2 = oe_anno_attr_color2;
        }

        widthBy2 = oe_anno_info.x * 0.5;
        heightBy2 = oe_anno_info.y * 0.5;
    }

    // Place label along circle
    // (FLT_MAX is used for standard labels)
    else if ( oe_anno_attr_circle_center.x < 10000000 )
    {
        float x = vertex.x * mp_local2pixel;
        // +8 so that the label is slightly shifted from the line
        float y = (oe_anno_info.y-vertex.y+8.) * mp_local2pixel;
        vec3 C = oe_anno_attr_circle_center;
        vec3 A = oe_anno_attr_circle_anchor;
        float r = length(A-C);
        float alpha = x / r;
        mat3 rot = rotAxis(C, alpha);
        vertex.xyz = rot * A.xyz;
        vertex.xyz = mix(vertex.xyz, C, y/r);
        vertex.xyz = normalize(vertex.xyz) * length(A);
    }
}

