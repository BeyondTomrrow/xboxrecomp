// NV2A pixel pipeline for the Direct3D 11 renderer (nv2a_gpu_d3d11.inc.c).
//
// Edit this file, then run tools/gen-gpu-hlsl.py to regenerate
// nv2a_gpu_d3d11_hlsl.inc (the C string the renderer compiles at startup).
//
// Texture stages follow xemu's pgraph/glsl/psh.c: each stage has a mode from
// SET_SHADER_STAGE_PROGRAM and may read an earlier stage (dot-product and
// dependent modes). The register combiners follow comb_eval in
// nv2a_pb_exec.c.

struct VIn {
    float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
    float4 wz : TEXCOORD4;   // x: oPos.w, for w-buffered depth
    float4 rawscreen : TEXCOORD5;
};
struct PIn {
    float4 pos : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
    float4 wz : TEXCOORD4;
    float4 rawscreen : TEXCOORD5;
    nointerpolation float4 tri0 : TEXCOORD6;
    nointerpolation float4 tri1 : TEXCOORD7;
    nointerpolation float4 tri2 : TEXCOORD8;
    nointerpolation float2 depthmeta : TEXCOORD9;
};
struct POut {
    float4 c : SV_Target;
    float  d : SV_Depth;
};

PIn vs_main(VIn v)
{
    PIn o;
    o.pos = v.pos; o.d0 = v.d0; o.d1 = v.d1;
    o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3;
    o.wz = v.wz;
    o.rawscreen = v.rawscreen;
    o.tri0 = o.tri1 = o.tri2 = 0;
    o.depthmeta = 0;
    return o;
}

cbuffer C : register(b0) {
    uint4  stg[8];        // combiner stages: color icw, ocw, alpha icw, ocw
    float4 f0[8];
    float4 f1[8];
    uint4  misc;          // control, stage_prog, final0, final1
    float4 sf0;
    float4 sf1;
    uint4  alpha;         // test enable, func, ref, combiner active
    uint4  texon;         // 1 when a texture is bound for the stage
    uint4  mode;          // texture stage mode per stage (PS_TEXTUREMODES_*)
    uint4  dotmap;        // dot mapping per stage (SET_DOT_RGBMAPPING)
    uint4  input;         // input stage per stage (SET_SHADER_OTHER_STAGE_INPUT)
    uint4  cube;          // 1 when the stage's texture is a cube map
    uint4  rect;          // 1 when the stage's coordinates are in texels
    float4 tsize[4];      // texture width, height per stage
    uint4  show;          // RECOMP_GPU_SHOW debug view (x): 0 normal, 1-4 t0-t3, 5 d0, 6 alpha
    float4 depthp;        // x: w-buffer (Z perspective), y: zmax, z/w: clip min/max
    float4 fogc;          // fog colour; the factor arrives in wz.y
    uint4 depthfmt;       // x: 0 floating depth, or integer depth bit count
    float4 depthofs;      // x: polygon bias, y: slope factor, z: render scale
};

// A specialised variant (gpu_ps_shader in nv2a_gpu_d3d11.inc.c) inserts,
// at the marker line below, compile-time copies of the combiner and texture
// stage state and #defines the cbuffer names above to them. The compiler then
// folds every mode test and resolves R[] to plain registers; the generic form
// (state read from the cbuffer, R[] indexed at run time) costs ~2 ns per
// pixel and made high internal resolutions GPU-bound (2026-10-03).
//@PS_SPEC@

// The geometry input consumes only the VS values, not the triangle metadata
// it creates. Keep that signature independent of the line fallback outputs.
struct GIn {
    float4 pos : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
    float4 wz : TEXCOORD4; float4 rawscreen : TEXCOORD5;
};

precise float triangle_area(float2 a, float2 b, float2 c)
{
    precise float2 e = b - a;
    precise float2 f = c - a;
    return e.x * f.y - e.y * f.x;
}

// Preserve the original Xbox triangle through host clipping. Reconstructing
// its depth in the pixel shader avoids the host's different plane rounding
// on equal-depth field/model overlays (xemu geom.c and psh.c).
[maxvertexcount(3)]
void gs_main(triangle GIn v[3], inout TriangleStream<PIn> stream)
{
    float4 p0 = v[0].rawscreen, p1 = v[1].rawscreen, p2 = v[2].rawscreen;
    precise float2 e1 = p1.xy - p0.xy, e2 = p2.xy - p0.xy;
    precise float b1 = p1.z - p0.z, b2 = p2.z - p0.z;
    if (depthp.x != 0.0) {
        b1 = (p0.w - p1.w) / (p1.w * p0.w);
        b2 = (p0.w - p2.w) / (p2.w * p0.w);
    }
    precise float det = e1.x * e2.y - e1.y * e2.x;
    precise float dx = (b1 * e2.y - b2 * e1.y) / det;
    precise float dy = (b2 * e1.x - b1 * e2.x) / det;
    float slope = max(abs(dx), abs(dy));
    if (isnan(slope) || isinf(slope)) slope = 0;
    // A corner far off screen or behind the eye: the rasteriser clips the
    // triangle, and the projected corners no longer describe the visible
    // part, so depth comes from the rasteriser's interpolation instead.
    float clipped = 0;
    [unroll] for (uint q = 0; q < 3; ++q) {
        float4 r = v[q].rawscreen;
        if (r.w <= 0.0 || abs(r.x) >= 1e6 || abs(r.y) >= 1e6) clipped = 1;
    }
    [unroll] for (uint k = 0; k < 3; ++k) {
        PIn o;
        o.pos = v[k].pos; o.d0 = v[k].d0; o.d1 = v[k].d1;
        o.t0 = v[k].t0; o.t1 = v[k].t1; o.t2 = v[k].t2; o.t3 = v[k].t3;
        o.wz = v[k].wz; o.rawscreen = v[k].rawscreen;
        o.tri0 = p0; o.tri1 = p1; o.tri2 = p2;
        o.depthmeta = clipped != 0 ? float2(0, 0) : float2(slope, 1);
        stream.Append(o);
    }
    stream.RestartStrip();
}

Texture2D   T0 : register(t0); Texture2D   T1 : register(t1);
Texture2D   T2 : register(t2); Texture2D   T3 : register(t3);
TextureCube Q0 : register(t4); TextureCube Q1 : register(t5);
TextureCube Q2 : register(t6); TextureCube Q3 : register(t7);
SamplerState S0 : register(s0); SamplerState S1 : register(s1);
SamplerState S2 : register(s2); SamplerState S3 : register(s3);

float4 tex2d(uint s, float2 uv)
{
    if (s == 0u) return T0.Sample(S0, uv);
    if (s == 1u) return T1.Sample(S1, uv);
    if (s == 2u) return T2.Sample(S2, uv);
    return T3.Sample(S3, uv);
}

float4 texcube(uint s, float3 d)
{
    if (s == 0u) return Q0.Sample(S0, d);
    if (s == 1u) return Q1.Sample(S1, d);
    if (s == 2u) return Q2.Sample(S2, d);
    return Q3.Sample(S3, d);
}

// A 2D lookup at coordinates given in the stage's convention: texels for a
// linear (rect) texture, normalised otherwise.
float4 tex2dc(uint s, float2 uv)
{
    if (rect[s] != 0u) uv /= tsize[s].xy;
    return tex2d(s, uv);
}

// Direction lookup: a cube map, or face-remapped 2D when the stage's texture
// is not one (xemu remapCubeTo2D).
float4 texdir(uint s, float3 d)
{
    if (cube[s] != 0u) return texcube(s, d);
    float3 a = max(abs(d), 1e-8);
    float2 uv;
    if (a.x > a.y && a.x > a.z) uv = (d.x > 0 ? float2(-d.z, d.y) : float2(d.z, d.y)) / a.x;
    else if (a.y > a.x && a.y > a.z) uv = (d.y > 0 ? float2(d.x, -d.z) : float2(d.x, d.z)) / a.y;
    else uv = (d.z > 0 ? float2(d.x, d.y) : float2(-d.x, d.y)) / a.z;
    return tex2d(s, uv * 0.5 + 0.5);
}

float sign1(float x) { x *= 255.0; return (x - 128.0) / 127.0; }
float sign2(float x) { x *= 255.0; return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5; }
float sign3(float x) { x *= 255.0; return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }

float3 dotmapf(uint m, float4 c)
{
    if (m == 1u) return float3(sign1(c.r), sign1(c.g), sign1(c.b));
    if (m == 2u) return float3(sign2(c.r), sign2(c.g), sign2(c.b));
    if (m == 3u) return float3(sign3(c.r), sign3(c.g), sign3(c.b));
    return c.rgb;       // 0: zero to one; the HILO modes are unimplemented, as in xemu
}

float mapin(float x, uint m)
{
    float p = max(x, 0.0);
    if (m == 0x00) return p;
    if (m == 0x20) return 1.0 - min(p, 1.0);
    if (m == 0x40) return 2.0 * p - 1.0;
    if (m == 0x60) return -2.0 * p + 1.0;
    if (m == 0x80) return p - 0.5;
    if (m == 0xA0) return -p + 0.5;
    if (m == 0xC0) return x;
    return -x;
}

float mapout(float x, uint m)
{
    if (m == 0x08) x -= 0.5;
    else if (m == 0x10) x *= 2.0;
    else if (m == 0x18) x = (x - 0.5) * 2.0;
    else if (m == 0x20) x *= 4.0;
    else if (m == 0x30) x *= 0.5;
    return clamp(x, -1.0, 1.0);
}

float3 inrgb(float4 R[16], uint b)
{
    uint r = b & 15u; bool a = (b & 0x10u) != 0u; uint m = b & 0xE0u;
    float3 x = a ? R[r].aaa : R[r].rgb;
    return float3(mapin(x.r, m), mapin(x.g, m), mapin(x.b, m));
}

float ina(float4 R[16], uint b)
{
    uint r = b & 15u; bool a = (b & 0x10u) != 0u;
    return mapin(a ? R[r].a : R[r].b, b & 0xE0u);
}

float4 shade(PIn i);

// Depth as the NV2A computes it (xemu psh.c): with Z perspective the
// perspective-correct w, otherwise the interpolated z; either way in
// depth-buffer units, clamped to the clip range, over the format's maximum.
POut ps_main(PIn i)
{
    POut r;
    // Depth in depth-buffer units: w with Z perspective, else z.
    precise float zd = depthp.x != 0.0 ? i.wz.x : i.pos.z * depthp.y;
    float slope = max(abs(ddx(zd)), abs(ddy(zd))) * depthofs.z;
    if (i.depthmeta.y != 0.0) {
        float2 p = i.pos.xy / depthofs.z;
        precise float bc0 = triangle_area(p, i.tri1.xy, i.tri2.xy);
        precise float bc1 = triangle_area(p, i.tri2.xy, i.tri0.xy);
        precise float bc2 = triangle_area(p, i.tri0.xy, i.tri1.xy);
        if (depthp.x != 0.0) {
            bc0 /= i.tri0.w; bc1 /= i.tri1.w; bc2 /= i.tri2.w;
        }
        float inv = 1.0 / (bc0 + bc1 + bc2);
        if (isinf(inv)) inv = 0.0;
        bc1 *= inv; bc2 *= inv;
        if (depthp.x != 0.0)
            zd = i.tri0.w + (bc1 * (i.tri1.w - i.tri0.w) + bc2 * (i.tri2.w - i.tri0.w));
        else
            zd = i.tri0.z + (bc1 * (i.tri1.z - i.tri0.z) + bc2 * (i.tri2.z - i.tri0.z));
        slope = i.depthmeta.x;
    }
    if (depthp.x != 0.0) {
        if (zd > 0.0) {
            precise float slope_offset = depthofs.y * slope * zd * zd;
            zd += depthofs.x;
            zd += slope_offset;
        }
        else zd = asfloat(0x7F7FFFFFu);
        if (isnan(zd)) zd = asfloat(0x7F7FFFFFu);
    } else zd += depthofs.x + depthofs.y * slope;
    // Outside the clip range: discarded, unless SET_ZMIN_MAX_CONTROL says
    // clamp (show.y = 0). xemu psh.c "Depth clipping".
    if (show.y != 0u && (zd < depthp.z || zd > depthp.w))
        discard;
    zd = clamp(zd, depthp.z, depthp.w);
    r.c = shade(i);
    // The host uses a float depth surface for all guest formats. Explicitly
    // floor Xbox integer depth before storing it: retaining fractional
    // values changes coplanar/equal-depth tests (decals and model overlays).
    // Floating Xbox depth keeps its fractional value.
    if (depthfmt.x != 0u) zd = floor(zd);
    r.d = zd / depthp.y;
    return r;
}

float4 shade(PIn i)
{
    float4 T[4] = { i.t0, i.t1, i.t2, i.t3 };
    float4 t[4] = { float4(0, 0, 0, 1), float4(0, 0, 0, 1), float4(0, 0, 0, 1), float4(0, 0, 0, 1) };
    float dt[4] = { 0, 0, 0, 0 };
    [unroll] for (uint s = 0; s < 4; s++) {
        uint m = mode[s];
        float4 pt = T[s];
        uint src = input[s];
        float4 prev = t[min(src, 3u)];
        if (s == 0u && m != 0u && texon[0] == 0u) m = 0xFFu;      // untextured stage 0
        if (m == 0u)                        t[s] = float4(0, 0, 0, 1);         // NONE
        else if (m == 0xFFu)                t[s] = float4(1, 1, 1, 1);
        else if (m == 1u || m == 2u || m == 6u || m == 7u) {               // PROJECT2D/3D, BUMPENVMAP (unimplemented: plain)
            float w = (m == 1u || m == 2u) && pt.w != 0.0 ? pt.w : 1.0;
            t[s] = texon[s] ? (cube[s] ? texcube(s, pt.xyz) : tex2dc(s, pt.xy / w)) : float4(1, 1, 1, 1);
        }
        else if (m == 3u)                   t[s] = texon[s] ? texdir(s, pt.xyz) : float4(1, 1, 1, 1);   // CUBEMAP
        else if (m == 4u)                   t[s] = pt;                          // PASSTHRU
        else if (m == 0x11u) {                                                   // DOTPRODUCT
            dt[s] = dot(pt.xyz, dotmapf(dotmap[s], prev));
            t[s] = float4(0, 0, 0, 0);
        }
        else if (m == 0x09u) {                                                   // DOT_ST
            dt[s] = dot(pt.xyz, dotmapf(dotmap[s], prev));
            t[s] = tex2dc(s, float2(dt[s - 1], dt[s]));
        }
        else if (m == 0x0Bu) {                                                   // DOT_RFLCT_DIFF (stage 2)
            dt[s] = dot(pt.xyz, dotmapf(dotmap[s], prev));
            float dn = dot(T[3].xyz, dotmapf(dotmap[3], t[min(input[3], 3u)]));
            t[s] = texdir(s, float3(dt[s - 1], dt[s], dn));
        }
        else if (m == 0x0Cu) {                                                   // DOT_RFLCT_SPEC (stage 3)
            dt[s] = dot(pt.xyz, dotmapf(dotmap[s], prev));
            float3 n = float3(dt[1], dt[2], dt[3]);
            float3 e = float3(T[1].w, T[2].w, T[3].w);
            float3 r = 2.0 * n * dot(n, e) / max(dot(n, n), 1e-8) - e;
            t[s] = texdir(s, r);
        }
        else if (m == 0x0Du || m == 0x0Eu) {                                     // DOT_STR_3D / CUBE
            dt[s] = dot(pt.xyz, dotmapf(dotmap[s], prev));
            t[s] = texdir(s, float3(dt[1], dt[2], dt[3]));
        }
        else if (m == 0x0Fu)                t[s] = tex2d(s, prev.ar);           // DEPENDENT_AR
        else if (m == 0x10u)                t[s] = tex2d(s, prev.gb);           // DEPENDENT_GB
        else                                t[s] = float4(0, 0, 0, 1);          // unimplemented
    }

    float4 d0 = saturate(i.d0), d1 = saturate(i.d1);
    float4 o;
    if (alpha.w == 0u) {
        o = (mode[0] != 0u && texon[0] != 0u) ? t[0] * d0 : d0;
    } else {
        float4 R[16];
        [unroll] for (int k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);
        R[3] = float4(fogc.rgb, saturate(i.wz.y));
        R[4] = d0; R[5] = d1;
        R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];
        R[12].a = mode[0] != 0u ? t[0].a : 1.0;
        uint ns = min(misc.x & 255u, 8u);
#ifdef PS_SPEC
        [unroll]
#endif
        for (uint cs = 0; cs < ns; cs++) {
            uint c0 = (misc.x & 0x1000u) ? cs : 0u;
            uint c1 = (misc.x & 0x10000u) ? cs : 0u;
            R[1] = f0[c0]; R[2] = f1[c1];
            float4 N[16];
            [unroll] for (int q = 0; q < 16; q++) N[q] = R[q];
            bool muxcd = (misc.x & 0x100u) ? (R[12].a >= 0.5)
                                           : ((uint(R[12].a * 255.0 + 0.5) & 1u) != 0u);
            {
                uint icw = stg[cs].x, ocw = stg[cs].y, fl = ocw >> 12, mp = fl & 0x38u;
                float3 A = inrgb(R, (icw >> 24) & 255u), B = inrgb(R, (icw >> 16) & 255u);
                float3 Cc = inrgb(R, (icw >> 8) & 255u), D = inrgb(R, icw & 255u);
                float3 ab = (fl & 2u) ? dot(A, B).xxx : A * B;
                float3 cd = (fl & 1u) ? dot(Cc, D).xxx : Cc * D;
                float3 ms = (fl & 4u) ? (muxcd ? cd : ab) : ab + cd;
                ab = float3(mapout(ab.x, mp), mapout(ab.y, mp), mapout(ab.z, mp));
                cd = float3(mapout(cd.x, mp), mapout(cd.y, mp), mapout(cd.z, mp));
                ms = float3(mapout(ms.x, mp), mapout(ms.y, mp), mapout(ms.z, mp));
                uint abd = (ocw >> 4) & 15u, cdd = ocw & 15u, msd = (ocw >> 8) & 15u;
                if (abd) { N[abd].rgb = ab; if (fl & 0x80u) N[abd].a = ab.b; }
                if (cdd) { N[cdd].rgb = cd; if (fl & 0x40u) N[cdd].a = cd.b; }
                if (msd) N[msd].rgb = ms;
            }
            {
                uint icw = stg[cs].z, ocw = stg[cs].w, fl = ocw >> 12, mp = fl & 0x38u;
                float A = ina(R, (icw >> 24) & 255u), B = ina(R, (icw >> 16) & 255u);
                float Cc = ina(R, (icw >> 8) & 255u), D = ina(R, icw & 255u);
                float ab = A * B, cd = Cc * D, ms = (fl & 4u) ? (muxcd ? cd : ab) : ab + cd;
                uint abd = (ocw >> 4) & 15u, cdd = ocw & 15u, msd = (ocw >> 8) & 15u;
                if (abd) N[abd].a = mapout(ab, mp);
                if (cdd) N[cdd].a = mapout(cd, mp);
                if (msd) N[msd].a = mapout(ms, mp);
            }
            [unroll] for (int q2 = 0; q2 < 16; q2++) R[q2] = N[q2];
        }
        if (misc.z != 0u || misc.w != 0u) {
            uint fl = misc.w & 255u;
            R[1] = sf0; R[2] = sf1;
            float3 E = inrgb(R, (misc.w >> 24) & 255u), F = inrgb(R, (misc.w >> 16) & 255u);
            float3 v1 = (fl & 0x40u) ? 1.0 - R[5].rgb : R[5].rgb;
            float3 r0 = (fl & 0x20u) ? 1.0 - R[12].rgb : R[12].rgb;
            float3 sum = v1 + r0;
            if (fl & 0x80u) sum = saturate(sum);
            R[14] = float4(sum, 0); R[15] = float4(E * F, 0);
            float3 A = inrgb(R, (misc.z >> 24) & 255u), B = inrgb(R, (misc.z >> 16) & 255u);
            float3 Cc = inrgb(R, (misc.z >> 8) & 255u), D = inrgb(R, misc.z & 255u);
            float G = ina(R, (misc.w >> 8) & 255u);
            o = float4(D + Cc * (1.0 - A) + B * A, G);
        } else {
            o = R[12];
        }
        o = saturate(o);
    }
    if (show.x >= 1u && show.x <= 4u) return float4(t[show.x - 1u].rgb, 1.0);
    if (show.x == 5u) return float4(d0.rgb, 1.0);
    if (show.x == 6u) return float4(o.aaa, 1.0);
    if (alpha.x != 0u) {
        uint a8 = (uint)(saturate(o.a) * 255.0 + 0.5), ref = alpha.z;
        bool ok = true;
        if (alpha.y == 0x200u) ok = false;
        else if (alpha.y == 0x201u) ok = a8 < ref;
        else if (alpha.y == 0x202u) ok = a8 == ref;
        else if (alpha.y == 0x203u) ok = a8 <= ref;
        else if (alpha.y == 0x204u) ok = a8 > ref;
        else if (alpha.y == 0x205u) ok = a8 != ref;
        else if (alpha.y == 0x206u) ok = a8 >= ref;
        if (!ok) discard;
    }
    return o;
}
