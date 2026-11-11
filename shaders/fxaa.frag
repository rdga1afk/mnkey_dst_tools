#version 450 core
// fxaa.frag -- FXAA 3.11 quality (T. Lottes, NVIDIA, public) as Kenshi uses it (data/materials/post/FXAA.hlsl, read at run time for the facts): preset 12 (5 search steps 1, 1.5, 2, 4, 12),
// green channel as luma, subpix 0.75, edge threshold 0.166, edge threshold min 0.0833, LDR input, bilinear sampling. Written from the algorithm, not copied. #180
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_scene;
layout(std140, set = 3, binding = 0) uniform FxaaUBO { vec2 u_rcp; vec2 _pad; };

const float kSubpix = 0.75, kEdgeThr = 0.166, kEdgeThrMin = 0.0833;
const float kStep[5] = float[5](1.0, 1.5, 2.0, 4.0, 12.0);

float lumaAt(vec2 p) { return texture(u_scene, p).g; }

void main() {
    vec2 posM = v_uv;
    vec4 rgbM = texture(u_scene, posM);
    float lumaM = rgbM.g;
    float lumaS = lumaAt(posM + vec2(0.0, u_rcp.y)), lumaE = lumaAt(posM + vec2(u_rcp.x, 0.0));
    float lumaN = lumaAt(posM - vec2(0.0, u_rcp.y)), lumaW = lumaAt(posM - vec2(u_rcp.x, 0.0));
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(kEdgeThrMin, rangeMax * kEdgeThr)) { out_color = vec4(rgbM.rgb, 1.0); return; }
    float lumaNW = lumaAt(posM + vec2(-u_rcp.x, -u_rcp.y)), lumaSE = lumaAt(posM + vec2(u_rcp.x, u_rcp.y));
    float lumaNE = lumaAt(posM + vec2(u_rcp.x, -u_rcp.y)), lumaSW = lumaAt(posM + vec2(-u_rcp.x, u_rcp.y));
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2), edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = u_rcp.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    if (horzSpan) lengthSign = u_rcp.y;
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);
    vec2 posB = posM;
    vec2 offNP = vec2(!horzSpan ? 0.0 : u_rcp.x, horzSpan ? 0.0 : u_rcp.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;
    vec2 posN = posB - offNP * kStep[0], posP = posB + offNP * kStep[0];
    float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = lumaAt(posN), lumaEndP = lumaAt(posP);
    float subpixE = subpixC * subpixC;
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5; lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * kStep[1];
    if (!doneP) posP += offNP * kStep[1];
    for (int i = 2; i < 5 && (!doneN || !doneP); ++i) {   // the unrolled steps P2..P4 of preset 12, each only while an end is still open
        if (!doneN) lumaEndN = lumaAt(posN) - lumaNN * 0.5;
        if (!doneP) lumaEndP = lumaAt(posP) - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled; doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN) posN -= offNP * kStep[i];
        if (!doneP) posP += offNP * kStep[i];
    }
    float dstN = posM.x - posN.x, dstP = posP.x - posM.x;
    if (!horzSpan) { dstN = posM.y - posN.y; dstP = posP.y - posM.y; }
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero, goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / (dstP + dstN);
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * (-spanLengthRcp) + 0.5;
    float subpixH = subpixG * kSubpix;
    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    out_color = vec4(texture(u_scene, posM).rgb, 1.0);
}
