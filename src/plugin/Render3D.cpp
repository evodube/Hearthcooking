// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Render3D.h"
#include "Config.h"
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <d3dcompiler.h>

namespace Render3D
{
	namespace
	{
		// ---------------------------------------------------------------- shaders
		// A warm key light from up and to the left with soft shadows, a hemisphere fill, and the fire: a point light that
		// flickers. ACES filmic tone mapping. Textures are the game's own (sRGB content in UNORM formats); normal maps
		// carry the specular mask in alpha, as Skyrim's do.
		constexpr const char* kShader = R"(
cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj;
	row_major float4x4 lightViewProj;
	float4 camPos;
	float4 keyDir;      // toward the light
	float4 keyColor;
	float4 sky;
	float4 ground;
	float4 firePos;     // w: reach
	float4 fireColor;
	float4 params;      // x time, y shadow texel, z exposure
	float4 camRight;
	float4 camUp;
	float4 occ[32];     // what's on the board as rough balls (middle, radius)
	float4 occInfo;     // x: how many
	float4 haze;        // heat shimmer rising from here (world), w: how strong
};
cbuffer Object : register(b1)
{
	float4 rot0, rot1, rot2;
	float4 posScale;
	float4 scale3;
	float4 spec;        // rgb * scale, w gloss
	float4 misc;        // x env scale, y alpha, z alpha ref (0..1), w alpha test
	uint4  has;         // diffuse, normal, env, env mask
	float4 tint;
	float4 lift;        // rgb, amount
	float4 emissive;
	float4 clips[6];
	float4 flesh;       // rgb, w on: cut faces
	float4 shape[4];    // middle (w power along x), axes over radii
	float4 uvx;         // scale, offset
	float4 cookA, cookB, cookC;
	float4 cookAxis;    // xyz, w mode
	float4 cookRef;
	float4 hollow;      // a hollow (a pot): its rim's middle, radius
	float4 hollow2;     // x: its depth (0: none)
	float4 fx;          // x lumps (dough not yet kneaded), y flour dusted on, z crust (blistered, unevenly browned), w wet glisten
	float4 spot;        // where flour's dusted (the board round the dough): middle, radius
	float4 fx2;         // x a liquid's surface rippling (by how hard it boils), y fat glinting on it
};
cbuffer Sprite : register(b3)
{
	float4 spPos;       // xyz, half-width
	float4 spColor;
	float4 spParams;    // kind, seed, rotation, stretch
};

Texture2D    texDiffuse : register(t0);
Texture2D    texNormal  : register(t1);
TextureCube  texEnv     : register(t2);
Texture2D    texEnvMask : register(t3);
Texture2D    texFlesh   : register(t5);
Texture2D    texShadow  : register(t4);
SamplerState smpWrap    : register(s0);
SamplerComparisonState smpShadow : register(s1);

struct VSIn
{
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
	float4 nrm : NORMAL;
	float4 tan : TANGENT;
};
struct VSOut
{
	float4 svpos : SV_Position;
	float3 wpos  : TEXCOORD0;
	float2 uv    : TEXCOORD1;
	float3 n     : TEXCOORD2;
	float3 t     : TEXCOORD3;
	float3 b     : TEXCOORD4;
	float3 opos  : TEXCOORD5;  // its own space, at world size (for its lumps, crust, flour)
};

float3 ToWorldDir(float3 v) { return float3(dot(rot0.xyz, v), dot(rot1.xyz, v), dot(rot2.xyz, v)); }
float3 ToWorld(float3 p) { return ToWorldDir(p * scale3.xyz) * posScale.w + posScale.xyz; }

VSOut VSMain(VSIn i)
{
	VSOut o;
	float3 wp = ToWorld(i.pos.xyz);
	o.svpos = mul(viewProj, float4(wp, 1));
	o.wpos = wp;
	o.uv = i.uv * uvx.xy + uvx.zw;
	o.n = ToWorldDir((i.nrm.xyz * 2 - 1) / scale3.xyz);
	o.t = ToWorldDir((i.tan.xyz * 2 - 1) * scale3.xyz);
	o.b = ToWorldDir(float3(i.pos.w, i.nrm.w * 2 - 1, i.tan.w * 2 - 1) * scale3.xyz);
	o.opos = i.pos.xyz * scale3.xyz * posScale.w;
	return o;
}

float4 VSShadow(VSIn i) : SV_Position
{
	return mul(lightViewProj, float4(ToWorld(i.pos.xyz), 1));
}

struct VSShadowOut { float4 svpos : SV_Position; float3 wpos : TEXCOORD0; };
VSShadowOut VSShadowCut(VSIn i)
{
	VSShadowOut o;
	o.wpos = ToWorld(i.pos.xyz);
	o.svpos = mul(lightViewProj, float4(o.wpos, 1));
	return o;
}

bool Clipped(float3 p)
{
	[unroll] for (int k = 0; k < 6; k++)
		if (any(clips[k].xyz != 0) && dot(clips[k].xyz, p) > clips[k].w) return true;
	return false;
}

void PSShadowCut(VSShadowOut i)
{
	if (Clipped(i.wpos)) discard;
}

float Shadow(float3 wp, float3 n)
{
	float4 lp = mul(lightViewProj, float4(wp + n * 0.08, 1));
	float2 uv = float2(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
	if (any(uv < 0) || any(uv > 1) || lp.z > 1) return 1;
	float s = 0, d = lp.z - 0.0012;
	[unroll] for (int y = -2; y <= 2; y++)
		[unroll] for (int x = -2; x <= 2; x++)
			s += texShadow.SampleCmpLevelZero(smpShadow, uv + float2(x, y) * params.y * 1.3, d);
	return s / 25;
}

float3 Lin(float3 c) { return pow(saturate(c), 2.2); }

)"
R"(
float Hash3(float3 p) { return frac(sin(dot(p, float3(127.1, 311.7, 74.7))) * 43758.5453); }
float Noise3(float3 x)
{
	float3 i = floor(x), f = frac(x);
	f = f * f * f * (f * (f * 6 - 15) + 10);  // (smooth: no grid showing)
	return lerp(lerp(lerp(Hash3(i), Hash3(i + float3(1, 0, 0)), f.x), lerp(Hash3(i + float3(0, 1, 0)), Hash3(i + float3(1, 1, 0)), f.x), f.y),
	            lerp(lerp(Hash3(i + float3(0, 0, 1)), Hash3(i + float3(1, 0, 1)), f.x), lerp(Hash3(i + float3(0, 1, 1)), Hash3(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float Fbm3(float3 x) { return Noise3(x) * 0.6 + Noise3(x * 2.1 + 7.3) * 0.28 + Noise3(x * 4.3 + 3.1) * 0.12; }

// Ambient occlusion, roughly: what's on the board shades what's under and beside it (each a ball: the light it hides
// from a surface, by how big it looks from there and how much it's overhead), and down inside a hollow (the pot)
// it's darker, deeper and toward the wall
float AO(float3 p, float3 n)
{
	float o = 0;
	int   cnt = (int)occInfo.x;
	[loop] for (int k = 0; k < cnt; k++) {
		float3 d = occ[k].xyz - p;
		float  l = length(d);
		float  r = occ[k].w;
		if (l < r * 1.05) continue;  // (on or in the ball: itself)
		o += saturate(dot(n, d / l) * 0.8 + 0.2) * (r * r) / (l * l);
	}
	float ao = 1 - saturate(o * 0.85);
	if (hollow2.x > 0.01) {
		float3 q = p - hollow.xyz;
		float  rr = length(q.xy);
		if (rr < hollow.w * 1.02 && q.z < 0.2) {
			float down = saturate(-q.z / hollow2.x);
			float wall = saturate(1 - (hollow.w - rr) / (hollow.w * 0.35));
			ao *= 1 - 0.38 * sqrt(down) - 0.22 * wall * (0.4 + 0.6 * down);
		}
	}
	return ao;
}

// Browning: golden at 1, dark at 1.4, black at 1.8; the texture's own light and shade kept as detail
float3 Cooked(float3 albedo, float c)
{
	if (c <= 0.02) return albedo;
	const float3 golden = float3(0.42, 0.21, 0.07), dark = float3(0.12, 0.055, 0.02), black = float3(0.018, 0.014, 0.012);
	if (emissive.w > 0.5) {
		// red meat seared: it darkens toward a deep brown-grey crust, keeping its own colour under it (no golden stage)
		float3 crust = albedo * float3(0.42, 0.33, 0.28);
		float3 s = lerp(albedo, crust, smoothstep(0.05, 1.0, c) * 0.92);
		s = lerp(s, albedo * float3(0.14, 0.1, 0.08), saturate((c - 1.0) / 0.45));
		return lerp(s, black, saturate((c - 1.45) / 0.4));
	}
	float  L = dot(albedo, float3(0.2126, 0.7152, 0.0722));
	float  detail = clamp(L / 0.28, 0.6, 1.4);
	float3 col = lerp(albedo, golden * detail, smoothstep(0.05, 1.0, c) * 0.88);
	col = lerp(col, dark * detail, saturate((c - 1.0) / 0.45));
	col = lerp(col, black, saturate((c - 1.45) / 0.4));
	return col;
}

float CookAt(float3 n)
{
	if (cookAxis.w < 0.5) return 0;
	if (cookAxis.w < 1.5) return cookA.x;
	// by side: which band this normal faces, round the axis
	float3 ax = normalize(cookAxis.xyz), e0 = normalize(cookRef.xyz), e1 = cross(ax, e0);
	float  along = abs(dot(n, ax));
	float  ang = atan2(dot(n, e1), dot(n, e0));
	float  f = frac(ang / 6.2831853) * 12.0;
	int    i0 = (int)floor(f) % 12, i1 = (i0 + 1) % 12;
	float  bands[12] = { cookA.x, cookA.y, cookA.z, cookA.w, cookB.x, cookB.y, cookB.z, cookB.w, cookC.x, cookC.y, cookC.z, cookC.w };
	float  b = lerp(bands[i0], bands[i1], frac(f));
	float  mean = (dot(cookA, 1) + dot(cookB, 1) + dot(cookC, 1)) / 12.0;
	return lerp(b, mean, saturate(along * 1.4 - 0.2));
}

// A cut piece: the inside of its skin, seen through a cut, is drawn as the cut face itself - flat, the flesh's colour,
// where the eye's ray first crosses into the kept part (so any number of cuts give a solid piece)
)"
R"(
bool CutFace(VSOut i, out float3 q, out float3 n)
{
	q = i.wpos;
	n = 0;
	float3 e = camPos.xyz, d = i.wpos - e;
	if (flesh.w < 0.5 || dot(normalize(i.n), -d) > 0) return false;  // its skin's outside: drawn as it is
	float best = -1;
	[unroll] for (int k = 0; k < 6; k++) {
		float3 pn = clips[k].xyz;
		if (all(pn == 0)) continue;
		float de = dot(pn, e) - clips[k].w, dd = dot(pn, d);
		if (de > 0 && dd < 0) {
			float t = -de / dd;
			if (t > best) { best = t; n = pn; }
		}
	}
	if (best < 0) return false;
	q = e + d * best;
	// only inside the whole thing's rough shape: a leaf or a stem seen from behind stays a leaf
	if (shape[1].x != 0 || shape[1].y != 0 || shape[1].z != 0) {
		float3 r = q - shape[0].xyz;
		float  lx = abs(dot(shape[1].xyz, r)), ly = dot(shape[2].xyz, r), lz = dot(shape[3].xyz, r);
		// a loose, boxy bound: an apple's cut faces reach well past the ellipsoid in its box, and were drawn as the
		// dark inside of the skin wherever they did; only a leaf or a stem stands this far out
		float ay = abs(ly), az = abs(lz);
		if (flesh.w > 1.5) {
			// a core: its cut face is exactly its own section, nothing past it - a round all along a leek or a carrot (an
			// ellipsoid narrows to nothing toward their ends, and the slices from there came out hollow)
			if (shape[0].w > 0.75 && shape[0].w < 1.5) {
				if (ly * ly + lz * lz > 1.02 || lx > 1.02) return false;
			} else if (lx * lx + ly * ly + lz * lz > 1.02) return false;
		} else if (shape[0].w < 0.75) {
			// a cabbage: a round head of leaves, its section a tight round too
			if (lx * lx + ay * ay + az * az > 1.0) return false;
		} else if (shape[0].w < 1.5) {
			// a long, thin thing (a leek, a carrot): its section is a round, tight to the stalk - its loose outer leaves,
			// fanned past it and seen from behind, were drawn as flat cut faces (big pale shards)
			if (ay * ay + az * az > 1.0 || lx > 1.02) return false;
		} else if (pow(lx, max(shape[0].w, 4.0)) + ay * ay * ay * ay + az * az * az * az > 1.6) {
			return false;
		}
	}
	return true;
}

)"
R"(
float4 Shade(VSOut i, bool cut, float3 q, float3 cn)
{
	if (cut) {
		// the flesh, faintly mottled by the skin's texture so it isn't a flat card
		float4 tx = has.x ? texDiffuse.Sample(smpWrap, q.xy * 0.37 + q.z * 0.21) : float4(0.5, 0.5, 0.5, 1);
		float  m = 0.9 + 0.2 * dot(tx.rgb, float3(0.33, 0.33, 0.34));
		float3 fc = Lin(flesh.rgb) * m;
		// its inside's own texture where it has one: the section across it, or along it, by which way the cut faces
		if (cookRef.w > 0.5 && any(shape[1].xyz != 0)) {
			float3 r = q - shape[0].xyz;
			float3 l = clamp(float3(dot(shape[1].xyz, r), dot(shape[2].xyz, r), dot(shape[3].xyz, r)), -1, 1);
			float3 a = abs(float3(dot(normalize(shape[1].xyz), cn), dot(normalize(shape[2].xyz), cn), dot(normalize(shape[3].xyz), cn)));
			float2 fu = a.x >= a.y && a.x >= a.z ? float2(0.25 + l.y * 0.245, 0.5 + l.z * 0.49)
			          : a.y >= a.z ? float2(0.75 + l.x * 0.245, 0.5 + l.z * 0.49) : float2(0.75 + l.x * 0.245, 0.5 + l.y * 0.49);
			fc = Lin(texFlesh.Sample(smpWrap, fu).rgb);
		}
		float3 alb = Cooked(lerp(fc * tint.rgb, Lin(lift.rgb), saturate(lift.a)), CookAt(cn));
		float3 V = normalize(camPos.xyz - q);
		float  nl = saturate(dot(cn, keyDir.xyz));
		float  sh = Shadow(q, cn);
		float3 hemi = lerp(ground.rgb, sky.rgb, cn.z * 0.5 + 0.5);
		float3 toF = firePos.xyz - q;
		float  df = length(toF);
		float  att = pow(saturate(1 - df / firePos.w), 2);
		float3 fire = fireColor.rgb * att * saturate(dot(cn, toF / max(df, 1e-3)) * 0.85 + 0.15);
		float3 H = normalize(keyDir.xyz + V);
		// a cut face is wet and pale: lit softly from every side, so the faces turned from the light don't go dark
		float  soft = 0.45 + 0.55 * saturate(dot(cn, keyDir.xyz) * 0.5 + 0.5);
		float3 col = alb * (hemi * 1.25 + keyColor.rgb * soft * lerp(0.6, 1.0, sh) + fire) + pow(saturate(dot(cn, H)), 40) * nl * sh * keyColor.rgb * 0.12;
		return float4(col, 1);
	}
	float4 dif = has.x ? texDiffuse.Sample(smpWrap, i.uv) : float4(0.6, 0.6, 0.6, 1);
	float a = dif.a * misc.y;
	if (misc.w > 0.5) clip(a - misc.z);
	// only blended meshes are see-through: many of the game's opaque textures keep something else in their alpha
	// (a plant's cabbage drew invisible when it counted)
	float alpha = scale3.w > 1.5 ? misc.y : scale3.w > 0.5 ? a : 1.0;
	float3 N0 = normalize(i.n);
	float3 albedo = Lin(dif.rgb) * tint.rgb;
	// a cut piece's skin seen from inside: its flesh, not the back of its peel
	bool inStalk = true;
	if (shape[0].w < 1.5 && any(shape[1].xyz != 0)) {
		float3 r = i.wpos - shape[0].xyz;
		float  ly = dot(shape[2].xyz, r), lz = dot(shape[3].xyz, r);
		float  lx = dot(shape[1].xyz, r);
		inStalk = ly * ly + lz * lz + (shape[0].w < 0.75 ? lx * lx : 0) <= 1.0;  // (a leek's or a cabbage's loose leaf, seen from behind, stays a leaf)
	}
	if (flesh.w > 2.5 && flesh.w < 3.5 && inStalk && dot(N0, camPos.xyz - i.wpos) < 0) {
		albedo = Lin(flesh.rgb) * 0.92 * tint.rgb;
	}
	// (a negative amount spares the fruit: red, purple and blue texels keep their colour, the pastry goes doughy)
	// (fruit: red, purple or blue, or dark - a juniper berry is nearly black; pastry is pale and yellow-brown)
	float  lum = dot(dif.rgb, float3(0.3, 0.55, 0.15));
	float  fruit = lift.a < 0 ? saturate(saturate((dif.b - dif.g) * 4) + saturate(((dif.r - dif.g) - 1.5 * (dif.g - dif.b)) * 3) + saturate((0.36 - lum) * 6)) : 0;
	albedo = lerp(albedo, Lin(lift.rgb), abs(lift.a) * (1 - fruit));
	// a crust browns unevenly, with blisters a shade darker
	float cookC = CookAt(N0);
	if (fx.z > 0.01 && cookC > 0.02) {
		float n1 = Fbm3(i.opos * 0.9);
		float bl = smoothstep(0.7, 0.86, Noise3(i.opos * 3.1));
		cookC *= lerp(1, 0.72 + 0.56 * n1, fx.z);
		cookC += bl * 0.25 * fx.z * saturate(cookC);
	}
	albedo = Cooked(albedo, cookC);
	// flour dusted on: speckled and patchy, on what faces up (round a spot, if it has one: the board round the dough)
	if (fx.y > 0.01) {
		float up = saturate(N0.z * 1.5 - 0.2);
		// soft drifts of it, finer grain through them (no hard specks)
		float cloud = smoothstep(0.32, 0.78, Fbm3(i.opos * 0.45));
		float grain = smoothstep(0.35, 0.75, Noise3(i.opos * 4.0));
		float speck = (cloud * 0.75 + grain * 0.3 * cloud + grain * 0.1) * 0.9;
		float sp = 1;
		if (spot.w > 0) {
			sp = saturate(1 - length(i.wpos.xy - spot.xy) / spot.w);
			sp *= sp;
		}
		albedo = lerp(albedo, Lin(float3(0.93, 0.91, 0.86)), saturate(fx.y * up * speck * sp));
	}
	float3 N = N0;
	float  mask = 1;
	if (has.y) {
		float4 nm = texNormal.Sample(smpWrap, i.uv);
		float3 t = normalize(i.t - N * dot(i.t, N));
		float3 b = normalize(i.b - N * dot(i.b, N));
		float3 d = nm.xyz * 2 - 1;
		N = normalize(d.x * t + d.y * b + d.z * N);
		mask = nm.a;
	}
	// a liquid's surface: rippling, more as it boils (slow swells and quick ripples, drifting)
	if (fx2.x > 0.01) {
		float3 q = float3(i.wpos.xy * 0.7, params.x * 0.8);
		float  e = 0.12, n0 = Fbm3(q);
		float2 g = float2(Fbm3(q + float3(e, 0, 0)) - n0, Fbm3(q + float3(0, e, 0)) - n0) / e;
		float3 q2 = float3(i.wpos.xy * 2.3 + params.x * 0.3, params.x * 2.1);
		float  m0 = Noise3(q2);
		g += float2(Noise3(q2 + float3(e, 0, 0)) - m0, Noise3(q2 + float3(0, e, 0)) - m0) / e * 0.35;
		N = normalize(N - float3(g, 0) * 0.22 * fx2.x);
	}
	// lumps: dough not yet kneaded smooth - soft swells and hollows (a few waves crossing, no grid), kneaded away
	float lumpH = 0;
	if (fx.x > 0.01) {
		const float3 dir[8] = { float3(0.83, 0.41, 0.38), float3(-0.35, 0.88, 0.31), float3(0.22, -0.47, 0.85), float3(-0.71, -0.52, 0.47),
		                        float3(0.56, -0.79, -0.24), float3(-0.12, 0.21, -0.97), float3(0.94, -0.08, 0.33), float3(-0.48, 0.67, -0.56) };
		const float  fr[8] = { 2.3, 2.6, 2.9, 3.3, 4.6, 5.2, 6.1, 7.4 };
		const float  ph[8] = { 0.3, 1.9, 4.2, 2.7, 5.1, 0.8, 3.6, 2.2 };
		float3 g = 0;
		[unroll] for (int k = 0; k < 8; k++) {
			float a = dot(i.opos, dir[k]) * fr[k] + ph[k];
			lumpH += sin(a) / fr[k] * 0.5;
			g += cos(a) * dir[k] * 0.5;
		}
		g = ToWorldDir(g);
		float3 gt = g - N * dot(g, N);
		float  gl = length(gt);
		gt *= min(1.0, 1.2 / max(gl, 1e-4));  // (a gentle tilt at most: steep ones went dark in odd patches)
		N = normalize(N - gt * 0.28 * fx.x);
		// the hollows a shade darker, flour caught on the swells
		albedo *= 1 - 0.12 * fx.x * saturate(-lumpH);
		albedo = lerp(albedo, Lin(float3(0.97, 0.96, 0.92)), saturate((lumpH - 0.2) * 2.5) * saturate(N0.z) * 0.55 * (0.4 + 0.6 * fx.x));
	}
	float3 V = normalize(camPos.xyz - i.wpos);
	if (dot(N, V) < -0.2) N = -N;  // an inside face seen from inside (the pot, the bowl)
	float3 L = keyDir.xyz;
	float  nl = saturate(dot(N, L));
	if (fx.x > 0.01) nl = lerp(nl, saturate(dot(N, L) * 0.6 + 0.4), fx.x);  // (soft dough: the light wraps round its lumps)
	float  sh = tint.w > 0.5 ? 1.0 : Shadow(i.wpos, normalize(i.n));  // (by the surface itself, not its bumps)
	float3 hemi = lerp(ground.rgb, sky.rgb, N.z * 0.5 + 0.5);
	float3 toF = firePos.xyz - i.wpos;
	float  df = length(toF);
	float3 Lf = toF / max(df, 1e-3);
	float  att = pow(saturate(1 - df / firePos.w), 2);
	// it flickers, more as it grows: a slow sway, quicker licks, and the light swaying side to side a little
	float  fa = 0.1 + 0.16 * saturate(fireColor.w);
	float  flick = 1.0 - fa + fa * (0.55 * sin(params.x * 7.3) + 0.3 * sin(params.x * 13.1 + 1.3) + 0.15 * sin(params.x * 29.7 + 0.4)) * 0.5 + fa * 0.5;
	Lf = normalize(Lf + float3(sin(params.x * 3.1), cos(params.x * 2.3), 0) * 0.08 * fa);
	float3 fire = fireColor.rgb * att * flick;
	float  nlf = saturate(dot(N, Lf) * 0.85 + 0.15);
	const float ao = 1;  // (the soft contact shading was taken out)
	float3 light = hemi * ao + keyColor.rgb * nl * sh * lerp(1, ao, 0.6) + fire * nlf * lerp(1, ao, 0.7);
	float3 H = normalize(L + V), Hf = normalize(Lf + V);
	float  g = max(spec.w, 1);
	float  norm = (g + 8) / 25.13;
	float3 sp = spec.rgb * mask * norm * (pow(saturate(dot(N, H)), g) * nl * sh * keyColor.rgb + pow(saturate(dot(N, Hf)), g) * nlf * fire);
	// fat on a liquid: drifting round glints, a little golden
	if (fx2.y > 0.01) {
		float fat = smoothstep(0.6, 0.72, Noise3(float3(i.opos.xy * 0.55, params.x * 0.04))) * fx2.y;
		float gl = pow(saturate(dot(N, H)), 50) * nl, glf = pow(saturate(dot(N, Hf)), 40) * nlf;
		sp += (keyColor.rgb * gl + fire * glf) * fat * 1.2;
		albedo *= 1 + fat * float3(0.1, 0.06, -0.04);
	}
	// wet and glistening (meat on the spit as its fat runs): a tight, bright highlight from the light and the fire
	if (fx.w > 0.01) {
		float gl = pow(saturate(dot(N, H)), 90) * nl * sh, glf = pow(saturate(dot(N, Hf)), 60) * nlf;
		sp += (keyColor.rgb * gl + fire * glf) * 1.4 * fx.w * (0.6 + 0.4 * Noise3(i.opos * 5.0));
	}
	float3 env = 0;
	if (has.z) {
		float3 R = reflect(-V, N);
		float  m = has.w ? texEnvMask.Sample(smpWrap, i.uv).r : mask;
		float  fres = 0.35 + 0.65 * pow(1 - saturate(dot(N, V)), 4);
		env = Lin(texEnv.SampleLevel(smpWrap, R, 2).rgb) * misc.x * m * fres * (hemi * 2.2 + keyColor.rgb * 0.25);
	}
	float3 col = albedo * light + sp + env + emissive.rgb;
	return float4(col * alpha, alpha);
}

float4 PSMain(VSOut i) : SV_Target
{
	if (Clipped(i.wpos)) discard;
	return Shade(i, false, i.wpos, 0);
}

// A cut piece's solid, drawn on a box round it: the eye's ray against the piece exactly - its cuts (planes) and the
// thing's own rounded shape (an ellipsoid) - so a piece from the middle is a solid cube however hollow the mesh is.
// A cut piece's face, where the eye's ray first meets one of its cuts inside the thing's box (its measured
// bounds, shape[] as axes over half-sizes). False where the ray comes in through the box's own sides (the skin's
// to draw) or misses the piece.
bool CapPoint(float3 wpos, out float3 q, out float3 n, out float tin)
{
	float3 e = camPos.xyz, d = normalize(wpos - camPos.xyz);
	float  tout = 1e9;
	tin = 0;
	n = float3(0, 0, 1);
	q = wpos;
	bool any = false;
	[unroll] for (int k = 0; k < 6; k++) {
		float3 pn = clips[k].xyz;
		if (all(pn == 0)) continue;
		float dd = dot(pn, d), de = dot(pn, e) - clips[k].w;
		if (abs(dd) < 1e-6) {
			if (de > 0) return false;
			continue;
		}
		float t = -de / dd;
		if (dd < 0) {
			if (t > tin) { tin = t; n = pn; any = true; }
		} else {
			tout = min(tout, t);
		}
	}
	if (!any) return false;
	float3 r = e - shape[0].xyz;
	float3 o = float3(dot(shape[1].xyz, r), dot(shape[2].xyz, r), dot(shape[3].xyz, r));
	float3 v = float3(dot(shape[1].xyz, d), dot(shape[2].xyz, d), dot(shape[3].xyz, d));
	[unroll] for (int a = 0; a < 3; a++) {
		if (abs(v[a]) < 1e-6) {
			if (abs(o[a]) > 1) return false;
			continue;
		}
		float t1 = (-1 - o[a]) / v[a], t2 = (1 - o[a]) / v[a];
		if (min(t1, t2) > tin) return false;  // in through the box's side: not a cut face
		tout = min(tout, max(t1, t2));
	}
	if (tin >= tout || tin <= 0) return false;
	q = e + d * tin;
	return true;
}

// Pass 1 of a cut piece's face: the thing's whole mesh (uncut), counting in the stencil how many of its surfaces
// lie between the eye and the cut face. An odd count: the face is inside the thing, and gets drawn.
float4 PSParity(VSOut i) : SV_Target
{
	float3 q, n;
	float  tin;
	if (!CapPoint(i.wpos, q, n, tin)) discard;
	if (length(i.wpos - camPos.xyz) >= tin - 1e-4) discard;
	return 0;
}

// ... and the surfaces beyond it (into the second bit): a point truly inside has an odd count both ways. Where the
// mesh is open (an apple's base, a cabbage's leaves) one count is odd and the other even, and no face is drawn.
float4 PSParityBack(VSOut i) : SV_Target
{
	float3 q, n;
	float  tin;
	if (!CapPoint(i.wpos, q, n, tin)) discard;
	if (length(i.wpos - camPos.xyz) < tin + 1e-4) discard;
	return 0;
}

// Pass 2: on a box round the piece, the face itself (only where pass 1 left an odd count)
float4 PSSolid(VSOut i, out float depth : SV_Depth) : SV_Target
{
	float3 q, n;
	float  tin;
	if (!CapPoint(i.wpos, q, n, tin)) discard;
	float4 cp = mul(viewProj, float4(q, 1));
	depth = cp.z / cp.w;
	return Shade(i, true, q, n);
}

float4 PSCut(VSOut i, out float depth : SV_Depth) : SV_Target
{
	if (Clipped(i.wpos)) discard;
	float3 q, n;
	bool   cut = CutFace(i, q, n);
	// a core shows only its cut faces: its own outside never (the thing's skin is the outside, whatever its shape)
	if (flesh.w > 1.5 && !cut) discard;
	float4 c = mul(viewProj, float4(q, 1));
	depth = c.z / c.w;
	return Shade(i, cut, q, n);
}

)"
R"(
// ---- sprites: fire, steam, bubbles, salt
struct SPOut { float4 svpos : SV_Position; float2 uv : TEXCOORD0; };
SPOut VSSprite(uint id : SV_VertexID)
{
	const float2 c[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
	float2 q = c[id];
	int    kind = (int)(spParams.x + 0.5);
	float3 right = camRight.xyz, up = camUp.xyz;
	if (kind == 0) { up = float3(0, 0, 1); }                       // flames stand upright
	if (kind == 4) { right = float3(1, 0, 0); up = float3(0, 1, 0); }  // bubbles lie on the broth
	float  cr = cos(spParams.z), sr = sin(spParams.z);
	float2 r = float2(q.x * cr - q.y * sr, q.x * sr + q.y * cr);
	float3 p = spPos.xyz + right * r.x * spPos.w + up * r.y * spPos.w * spParams.w;
	SPOut o;
	o.svpos = mul(viewProj, float4(p, 1));
	o.uv = q;
	return o;
}

float Hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float Noise(float2 p)
{
	float2 i = floor(p), f = frac(p);
	f = f * f * (3 - 2 * f);
	return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), f.x), f.y);
}
float Fbm(float2 p) { return Noise(p) * 0.55 + Noise(p * 2.03 + 1.7) * 0.3 + Noise(p * 4.1 + 3.1) * 0.15; }

float4 PSSprite(SPOut i) : SV_Target
{
	int   kind = (int)(spParams.x + 0.5);
	float t = params.x, seed = spParams.y;
	float2 uv = i.uv;
	float  r = length(uv);
	if (kind == 0) {
		// a tongue of flame: wide and hot at the foot, licking up and thinning, torn by rising noise
		float h = uv.y * 0.5 + 0.5;
		float x = uv.x + (Noise(float2(h * 3.0 - t * 2.6, seed * 7.0)) - 0.5) * 0.55 * h;
		float w = 0.8 * sqrt(saturate(h * 5.0)) * pow(saturate(1.0 - h), 0.75) + 0.02;
		float shape = saturate(1.0 - abs(x) / w);
		float n = Fbm(float2(x * 2.2 + seed, h * 2.4 - t * 3.2));
		float k = pow(shape, 1.4) * saturate(0.35 + n * 0.95) * saturate((1.0 - h) * 2.2);
		float core = pow(shape, 3.0) * saturate(1.2 - h * 1.6);
		float3 col = lerp(float3(0.9, 0.16, 0.02), float3(1.0, 0.5, 0.1), saturate(k * 1.6)) + float3(1.0, 0.85, 0.55) * core * 1.6;
		return float4(col * k * spColor.rgb * spColor.a * 2.2, 0);
	}
	if (kind == 1) {
		// a soft wisp: its shape warped by slow-turning noise, torn at the edges, curling as it rises
		float2 w = uv + (float2(Fbm(uv * 1.3 + float2(seed, seed * 0.7 - t * 0.3)), Fbm(uv * 1.3 + float2(seed * 1.7 + t * 0.2, seed))) - 0.5) * 0.9;
		float  n = Fbm(w * 2.2 + float2(seed * 5.1, seed * 2.3 - t * 0.25));
		float  edge = pow(saturate(1.0 - length(w) * 0.95), 1.4);
		float  a = edge * saturate(n * 1.7 - 0.3) * spColor.a;
		return float4(spColor.rgb * a, a);
	}
	if (kind == 2) {
		float a = pow(saturate(1.0 - r), 2.5) * spColor.a;
		return float4(spColor.rgb * a * 3.0, 0);
	}
	if (kind == 3) {
		float a = pow(saturate(1.0 - r), 2.0) * spColor.a;
		return float4(spColor.rgb * a, 0);
	}
	if (kind == 4) {
		float ring = exp(-pow((r - 0.72) / 0.13, 2));
		float hi = exp(-dot(uv - float2(-0.3, 0.3), uv - float2(-0.3, 0.3)) / 0.03);
		float a = saturate(ring * 0.55 + hi * 0.8) * spColor.a * step(r, 1.0);
		return float4(spColor.rgb * a, a);
	}
	float a = saturate((1.0 - r) * 3.0) * spColor.a;
	return float4(spColor.rgb * a, a);
}

// ---- full-screen passes
struct FSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FSOut VSFull(uint id : SV_VertexID)
{
	FSOut o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
SamplerState smpClamp : register(s2);
cbuffer PassCB : register(b2) { float4 passArgs; };  // xy texel, z amount, w direction/strength

float4 PSDown(FSOut i) : SV_Target
{
	float2 t = passArgs.xy;
	float3 c = texDiffuse.Sample(smpClamp, i.uv + t * float2(-1, -1)).rgb + texDiffuse.Sample(smpClamp, i.uv + t * float2(1, -1)).rgb +
	           texDiffuse.Sample(smpClamp, i.uv + t * float2(-1, 1)).rgb + texDiffuse.Sample(smpClamp, i.uv + t * float2(1, 1)).rgb;
	return float4(c * 0.25, 1);
}
float4 PSBlur(FSOut i) : SV_Target
{
	const float w[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
	float2 d = passArgs.w > 0.5 ? float2(0, passArgs.y) : float2(passArgs.x, 0);
	float3 c = texDiffuse.Sample(smpClamp, i.uv).rgb * w[0];
	[unroll] for (int k = 1; k < 5; k++)
		c += (texDiffuse.Sample(smpClamp, i.uv + d * k * 1.6).rgb + texDiffuse.Sample(smpClamp, i.uv - d * k * 1.6).rgb) * w[k];
	return float4(c, 1);
}
float4 PSBack(FSOut i) : SV_Target
{
	float3 c = texDiffuse.Sample(smpClamp, i.uv).rgb;
	float  l = dot(c, float3(0.299, 0.587, 0.114));
	c = lerp(float3(l, l, l), c, 0.72) * 0.62;
	return float4(c, passArgs.z);
}
float3 ACES(float3 c)
{
	const float3x3 inM = { 0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777 };
	const float3x3 outM = { 1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602 };
	c = mul(c, inM);
	float3 a = c * (c + 0.0245786) - 0.000090537;
	float3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
	c = mul(a / b, outM);
	return saturate(c);
}
float4 PSComposite(FSOut i) : SV_Target
{
	float2 uv = i.uv;
	// heat shimmer: the air over the fire (the oven's mouth) wavering, rising
	if (haze.w > 0.01) {
		float4 hp = mul(viewProj, float4(haze.xyz, 1));
		if (hp.w > 0.01) {
			float2 hc = hp.xy / hp.w * float2(0.5, -0.5) + 0.5;
			float2 d = (uv - hc) / float2(0.07, 0.11);
			d.y += 0.55;  // (above it)
			float  wgt = exp(-dot(d, d)) * haze.w;
			float  tt = params.x;
			float2 n = float2(Noise(uv * float2(60, 30) + float2(0, tt * 3.0)) - 0.5, Noise(uv * float2(45, 22) + float2(5.2, tt * 2.4)) - 0.5);
			uv += n * 0.006 * wgt;
		}
	}
	float4 c = texDiffuse.Sample(smpClamp, uv);
	if (c.a <= 0.0001) return 0;
	float3 rgb = c.rgb / c.a;
	rgb = ACES(rgb * passArgs.w / 0.6);
	rgb = pow(rgb, 1 / 2.2);
	float a = saturate(c.a) * passArgs.z;
	return float4(rgb * a, a);
}


)";

		struct CBFrame
		{
			float viewProj[16];
			float lightViewProj[16];
			float camPos[4];
			float keyDir[4];
			float keyColor[4];
			float sky[4];
			float ground[4];
			float firePos[4];
			float fireColor[4];
			float params[4];
			float camRight[4];
			float camUp[4];
			float occ[32][4];
			float occInfo[4];
			float haze[4];
		};
		struct CBObject
		{
			float         rot0[4], rot1[4], rot2[4];
			float         posScale[4];
			float         scale3[4];
			float         spec[4];
			float         misc[4];
			std::uint32_t has[4];
			float         tint[4];
			float         lift[4];
			float         emissive[4];
			float         clips[6][4];
			float         flesh[4];
			float         shape[4][4];
			float         uvx[4];
			float         cook[12];
			float         cookAxis[4];
			float         cookRef[4];
			float         hollow[4];
			float         hollow2[4];
			float         fx[4];
			float         spot[4];
			float         fx2[4];
		};
		struct CBSprite
		{
			float pos[4];
			float color[4];
			float params[4];
		};

		bool HasClips(const Item& a_it)
		{
			for (const auto& p : a_it.clips) {
				if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
					return true;
				}
			}
			return false;
		}

		template <class T>
		void Release(T*& a_p)
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		std::mutex    g_mtx;
		Frame         g_frame;
		bool          g_failed = false, g_ready = false;
		ID3D11Device* g_dev = nullptr;

		ID3D11VertexShader* g_vs = nullptr;
		ID3D11VertexShader* g_vsShadow = nullptr;
		ID3D11VertexShader* g_vsFull = nullptr;
		ID3D11VertexShader* g_vsSprite = nullptr;
		ID3D11PixelShader*  g_ps = nullptr;
		ID3D11PixelShader*  g_psCut = nullptr;
		ID3D11PixelShader*  g_psSolid = nullptr;
		ID3D11PixelShader*  g_psParity = nullptr;
		ID3D11PixelShader*  g_psParityBack = nullptr;
		ID3D11DepthStencilState* g_dsParity = nullptr;
		ID3D11DepthStencilState* g_dsParityBack = nullptr;
		ID3D11DepthStencilState* g_dsCap = nullptr;
		ID3D11BlendState*        g_bsNoColor = nullptr;
		ID3D11VertexShader* g_vsShadowCut = nullptr;
		ID3D11PixelShader*  g_psShadowCut = nullptr;
		ID3D11PixelShader*  g_psSprite = nullptr;
		ID3D11PixelShader*  g_psDown = nullptr;
		ID3D11PixelShader*  g_psBlur = nullptr;
		ID3D11PixelShader*  g_psBack = nullptr;
		ID3D11PixelShader*  g_psComposite = nullptr;
		std::vector<char>   g_vsCode;
		ID3D11Buffer*       g_cbFrame = nullptr;
		ID3D11Buffer*       g_cbObject = nullptr;
		ID3D11Buffer*       g_cbPass = nullptr;
		ID3D11Buffer*       g_cbSprite = nullptr;
		ID3D11Buffer*       g_dummyVB = nullptr;
		ID3D11SamplerState* g_smpWrap = nullptr;
		ID3D11SamplerState* g_smpShadow = nullptr;
		ID3D11SamplerState* g_smpClamp = nullptr;
		ID3D11RasterizerState*   g_rsNoCull = nullptr;
		ID3D11RasterizerState*   g_rsShadow = nullptr;
		ID3D11DepthStencilState* g_dsOn = nullptr;
		ID3D11DepthStencilState* g_dsRead = nullptr;
		ID3D11DepthStencilState* g_dsOff = nullptr;
		ID3D11BlendState*        g_bsOpaque = nullptr;
		ID3D11BlendState*        g_bsPremul = nullptr;
		ID3D11BlendState*        g_bsAlpha = nullptr;
		ID3D11BlendState*        g_bsAdd = nullptr;
		std::unordered_map<std::uint64_t, ID3D11InputLayout*> g_layouts;

		// size-dependent targets
		UINT                      g_w = 0, g_h = 0, g_msaa = 1, g_msaaWant = 0;
		int                       g_blurMade = 0;  // frames of the frozen world blurred so far
		DXGI_FORMAT               g_backFmt = DXGI_FORMAT_UNKNOWN;
		ID3D11Texture2D*          g_color = nullptr;  // MSAA, linear HDR, premultiplied
		ID3D11RenderTargetView*   g_colorRTV = nullptr;
		ID3D11Texture2D*          g_depth = nullptr;
		ID3D11DepthStencilView*   g_depthDSV = nullptr;
		ID3D11Texture2D*          g_resolved = nullptr;
		ID3D11ShaderResourceView* g_resolvedSRV = nullptr;
		ID3D11Texture2D*          g_bg = nullptr;  // a copy of the frame
		ID3D11ShaderResourceView* g_bgSRV = nullptr;
		ID3D11Texture2D*          g_q[2]{};  // quarter size, for the blur
		ID3D11RenderTargetView*   g_qRTV[2]{};
		ID3D11ShaderResourceView* g_qSRV[2]{};
		UINT                      kShadow = 2048;  // (by the quality setting, when the device is first set up)
		ID3D11Texture2D*          g_shadow = nullptr;
		ID3D11DepthStencilView*   g_shadowDSV = nullptr;
		ID3D11ShaderResourceView* g_shadowSRV = nullptr;

		// the meshes waiting to be measured, and what came back
		struct MeasureJob
		{
			std::uint64_t id;
			ID3D11Buffer* vb;
			std::uint64_t desc;
			std::uint32_t count;
		};
		std::mutex                                              g_measureMtx;
		std::vector<MeasureJob>                                 g_measureJobs;
		// What came back, by vertex buffer: each keeps its buffer alive, so the game can't free it and hand the same
		// address to another mesh while we still answer for it (a new kitchen's apple came out the size of an old cap)
		struct Measured
		{
			std::array<float, 6> box;
			ID3D11Buffer*        vb = nullptr;
		};
		std::unordered_map<std::uint64_t, Measured> g_measured;

		ID3DBlob* CompileNow(const char* a_entry, const char* a_target);
		// the shaders compiled ahead, off the main thread, while the game loads (they took a moment the first time the
		// kitchen opened): each taken from here once it's in, or compiled then if it isn't
		std::mutex                                   g_preMtx;
		std::unordered_map<std::string, ID3DBlob*>   g_pre;
		std::atomic<int>                             g_preState{ 0 };  // 0 not started, 1 compiling, 2 done
		ID3DBlob* Compile(const char* a_entry, const char* a_target)
		{
			while (g_preState.load() == 1) {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));  // (long done by the time the kitchen opens)
			}
			{
				std::lock_guard lk(g_preMtx);
				if (auto it = g_pre.find(a_entry); it != g_pre.end()) {
					auto* b = it->second;
					g_pre.erase(it);
					if (b) {
						return b;
					}
				}
			}
			return CompileNow(a_entry, a_target);
		}
		ID3DBlob* CompileNow(const char* a_entry, const char* a_target)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* err = nullptr;
			const HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "Hearthcooking", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
			if (FAILED(hr)) {
				spdlog::error("Kitchen renderer: shader {} failed: {}", a_entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
				Release(err);
				Release(code);
				return nullptr;
			}
			Release(err);
			return code;
		}

		bool Init(ID3D11Device* a_dev)
		{
			g_dev = a_dev;
			auto vs = [&](const char* e, ID3D11VertexShader** out, bool keep) {
				auto* b = Compile(e, "vs_5_0");
				if (!b) {
					return false;
				}
				const bool ok = SUCCEEDED(a_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
				if (keep) {
					g_vsCode.assign(static_cast<const char*>(b->GetBufferPointer()), static_cast<const char*>(b->GetBufferPointer()) + b->GetBufferSize());
				}
				b->Release();
				return ok;
			};
			auto ps = [&](const char* e, ID3D11PixelShader** out) {
				auto* b = Compile(e, "ps_5_0");
				if (!b) {
					return false;
				}
				const bool ok = SUCCEEDED(a_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
				b->Release();
				return ok;
			};
			if (!vs("VSMain", &g_vs, true) || !vs("VSShadow", &g_vsShadow, false) || !vs("VSShadowCut", &g_vsShadowCut, false) || !vs("VSFull", &g_vsFull, false) || !vs("VSSprite", &g_vsSprite, false) ||
				!ps("PSMain", &g_ps) || !ps("PSCut", &g_psCut) || !ps("PSSolid", &g_psSolid) || !ps("PSParity", &g_psParity) || !ps("PSParityBack", &g_psParityBack) || !ps("PSShadowCut", &g_psShadowCut) || !ps("PSSprite", &g_psSprite) || !ps("PSDown", &g_psDown) || !ps("PSBlur", &g_psBlur) || !ps("PSBack", &g_psBack) ||
				!ps("PSComposite", &g_psComposite)) {
				return false;
			}
			auto cb = [&](UINT size, ID3D11Buffer** out) {
				D3D11_BUFFER_DESC d{};
				d.ByteWidth = (size + 15) & ~15u;
				d.Usage = D3D11_USAGE_DYNAMIC;
				d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				return SUCCEEDED(a_dev->CreateBuffer(&d, nullptr, out));
			};
			if (!cb(sizeof(CBFrame), &g_cbFrame) || !cb(sizeof(CBObject), &g_cbObject) || !cb(16, &g_cbPass) || !cb(sizeof(CBSprite), &g_cbSprite)) {
				return false;
			}
			// stand-ins for attributes a mesh doesn't have: no UV, a normal facing up, a tangent along x
			{
				const std::uint8_t dummy[16] = { 0, 0, 0, 0, 128, 128, 255, 128, 255, 128, 128, 128, 0, 0, 0, 0 };
				D3D11_BUFFER_DESC  d{};
				d.ByteWidth = sizeof(dummy);
				d.Usage = D3D11_USAGE_IMMUTABLE;
				d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				D3D11_SUBRESOURCE_DATA s{ dummy, 0, 0 };
				if (FAILED(a_dev->CreateBuffer(&d, &s, &g_dummyVB))) {
					return false;
				}
			}
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_ANISOTROPIC;
			sd.MaxAnisotropy = 8;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			a_dev->CreateSamplerState(&sd, &g_smpWrap);
			sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
			sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
			sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
			sd.MaxAnisotropy = 1;
			a_dev->CreateSamplerState(&sd, &g_smpShadow);
			sd = {};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			a_dev->CreateSamplerState(&sd, &g_smpClamp);
			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			rd.MultisampleEnable = TRUE;
			a_dev->CreateRasterizerState(&rd, &g_rsNoCull);
			rd.MultisampleEnable = FALSE;
			rd.DepthBias = 300;
			rd.SlopeScaledDepthBias = 2.0f;
			a_dev->CreateRasterizerState(&rd, &g_rsShadow);
			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			a_dev->CreateDepthStencilState(&dd, &g_dsOn);
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			a_dev->CreateDepthStencilState(&dd, &g_dsRead);
			dd.DepthEnable = FALSE;
			a_dev->CreateDepthStencilState(&dd, &g_dsOff);
			// a cut piece's face: count the thing's surfaces before it (flip the low bit), then draw it where odd
			{
				D3D11_DEPTH_STENCIL_DESC sd{};
				sd.DepthEnable = FALSE;
				sd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
				sd.DepthFunc = D3D11_COMPARISON_ALWAYS;
				sd.StencilEnable = TRUE;
				sd.StencilReadMask = 1;
				sd.StencilWriteMask = 1;
				sd.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INVERT, D3D11_COMPARISON_ALWAYS };
				sd.BackFace = sd.FrontFace;
				a_dev->CreateDepthStencilState(&sd, &g_dsParity);
				sd.StencilReadMask = 2;
				sd.StencilWriteMask = 2;
				a_dev->CreateDepthStencilState(&sd, &g_dsParityBack);
				sd.StencilReadMask = 1;
				sd.StencilWriteMask = 1;
				sd.DepthEnable = TRUE;
				sd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
				sd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
				// drawn where both counts are odd; everything it covers is cleared, drawn or not
				sd.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_ZERO, D3D11_COMPARISON_EQUAL };
				sd.BackFace = sd.FrontFace;
				a_dev->CreateDepthStencilState(&sd, &g_dsCap);
			}
			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			a_dev->CreateBlendState(&bd, &g_bsOpaque);
			bd.RenderTarget[0].RenderTargetWriteMask = 0;
			a_dev->CreateBlendState(&bd, &g_bsNoColor);
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			auto& rt = bd.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D11_BLEND_ONE;
			rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D11_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D11_BLEND_ONE;
			rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			a_dev->CreateBlendState(&bd, &g_bsPremul);
			// light added on top (fire, embers): the colour adds, the coverage stays as it was
			rt.SrcBlend = D3D11_BLEND_ONE;
			rt.DestBlend = D3D11_BLEND_ONE;
			rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
			rt.DestBlendAlpha = D3D11_BLEND_ONE;
			a_dev->CreateBlendState(&bd, &g_bsAdd);
			rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
			rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
			rt.DestBlendAlpha = D3D11_BLEND_ONE;
			a_dev->CreateBlendState(&bd, &g_bsAlpha);
			// the shadow map
			kShadow = Config::Get().quality >= 2 ? 2048 : 1024;
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = kShadow;
			td.MipLevels = td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32_TYPELESS;
			td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(a_dev->CreateTexture2D(&td, nullptr, &g_shadow))) {
				return false;
			}
			D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
			dsv.Format = DXGI_FORMAT_D32_FLOAT;
			dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			a_dev->CreateDepthStencilView(g_shadow, &dsv, &g_shadowDSV);
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_FLOAT;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			a_dev->CreateShaderResourceView(g_shadow, &srv, &g_shadowSRV);
			spdlog::info("Kitchen renderer ready");
			return g_shadowDSV && g_shadowSRV;
		}

		void FreeTargets()
		{
			Release(g_colorRTV);
			Release(g_color);
			Release(g_depthDSV);
			Release(g_depth);
			Release(g_resolvedSRV);
			Release(g_resolved);
			Release(g_bgSRV);
			Release(g_bg);
			for (int k = 0; k < 2; ++k) {
				Release(g_qRTV[k]);
				Release(g_qSRV[k]);
				Release(g_q[k]);
			}
			g_w = g_h = 0;
		}

		DXGI_FORMAT Typed(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default:
				return f;
			}
		}

		bool Targets(const D3D11_TEXTURE2D_DESC& a_back)
		{
			const UINT want = Config::Get().quality >= 2 ? 4 : Config::Get().quality == 1 ? 2 : 1;
			if (g_w == a_back.Width && g_h == a_back.Height && g_backFmt == a_back.Format && g_color && g_msaaWant == want) {
				return true;
			}
			g_msaaWant = want;
			FreeTargets();
			g_blurMade = 0;
			g_w = a_back.Width;
			g_h = a_back.Height;
			g_backFmt = a_back.Format;
			UINT q = 0;
			g_msaa = want;
			if (g_msaa > 1 && (FAILED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, g_msaa, &q)) || q == 0)) {
				g_msaa = 1;
			}
			D3D11_TEXTURE2D_DESC td{};
			td.Width = g_w;
			td.Height = g_h;
			td.MipLevels = td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			td.SampleDesc.Count = g_msaa;
			td.BindFlags = D3D11_BIND_RENDER_TARGET;
			bool ok = SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_color)) && SUCCEEDED(g_dev->CreateRenderTargetView(g_color, nullptr, &g_colorRTV));
			td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;  // a stencil, for the faces of cut pieces
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_depth)) && SUCCEEDED(g_dev->CreateDepthStencilView(g_depth, nullptr, &g_depthDSV));
			td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_resolved)) && SUCCEEDED(g_dev->CreateShaderResourceView(g_resolved, nullptr, &g_resolvedSRV));
			D3D11_TEXTURE2D_DESC bd = a_back;
			bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bd.MiscFlags = 0;
			bd.CPUAccessFlags = 0;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.SampleDesc.Count = 1;
			bd.SampleDesc.Quality = 0;
			bd.MipLevels = 1;
			D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
			sv.Format = Typed(a_back.Format);
			sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sv.Texture2D.MipLevels = 1;
			ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&bd, nullptr, &g_bg)) && SUCCEEDED(g_dev->CreateShaderResourceView(g_bg, &sv, &g_bgSRV));
			td.Width = (std::max)(1u, g_w / 4);
			td.Height = (std::max)(1u, g_h / 4);
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			for (int k = 0; k < 2; ++k) {
				ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_q[k])) && SUCCEEDED(g_dev->CreateRenderTargetView(g_q[k], nullptr, &g_qRTV[k])) &&
				     SUCCEEDED(g_dev->CreateShaderResourceView(g_q[k], nullptr, &g_qSRV[k]));
			}
			if (!ok) {
				spdlog::error("Kitchen renderer: couldn't make its {}x{} targets (back buffer format {})", g_w, g_h, static_cast<int>(a_back.Format));
				FreeTargets();
				return false;
			}
			spdlog::info("Kitchen renderer: {}x{}, {}x MSAA, back buffer format {}", g_w, g_h, g_msaa, static_cast<int>(a_back.Format));
			return true;
		}

		// Where each attribute sits in a vertex, from the BSGraphics::VertexDesc
		UINT AttrOffset(std::uint64_t a_desc, int a_attr) { return static_cast<UINT>((a_desc >> (4 * a_attr + 2)) & 0x3C); }
		bool FullPrecision(std::uint64_t a_desc)
		{
			const auto flags = static_cast<std::uint16_t>(a_desc >> 44);
			// full precision positions take 16 bytes (the flag isn't always set on the renderer's copy): see where the next
			// attribute starts
			UINT next = static_cast<UINT>(a_desc & 0xF) * 4;
			for (int attr : { 1, 3, 4, 5 }) {
				if (flags & (1 << attr)) {
					next = (std::min)(next, AttrOffset(a_desc, attr));
				}
			}
			return (flags & 0x400) != 0 || next >= 16;
		}

		ID3D11InputLayout* Layout(std::uint64_t a_desc)
		{
			if (auto it = g_layouts.find(a_desc); it != g_layouts.end()) {
				return it->second;
			}
			const auto flags = static_cast<std::uint16_t>(a_desc >> 44);
			const bool full = FullPrecision(a_desc);
			D3D11_INPUT_ELEMENT_DESC e[4]{};
			e[0] = { "POSITION", 0, full ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, AttrOffset(a_desc, 0), D3D11_INPUT_PER_VERTEX_DATA, 0 };
			e[1] = (flags & (1 << 1)) ? D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, AttrOffset(a_desc, 1), D3D11_INPUT_PER_VERTEX_DATA, 0 }
			                          : D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			e[2] = (flags & (1 << 3)) ? D3D11_INPUT_ELEMENT_DESC{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, AttrOffset(a_desc, 3), D3D11_INPUT_PER_VERTEX_DATA, 0 }
			                          : D3D11_INPUT_ELEMENT_DESC{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			e[3] = (flags & (1 << 4)) ? D3D11_INPUT_ELEMENT_DESC{ "TANGENT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, AttrOffset(a_desc, 4), D3D11_INPUT_PER_VERTEX_DATA, 0 }
			                          : D3D11_INPUT_ELEMENT_DESC{ "TANGENT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			ID3D11InputLayout* il = nullptr;
			if (FAILED(g_dev->CreateInputLayout(e, 4, g_vsCode.data(), g_vsCode.size(), &il))) {
				spdlog::warn("Kitchen renderer: no input layout for vertex format {:016X}", a_desc);
			}
			g_layouts[a_desc] = il;
			return il;
		}

		float Half(std::uint16_t h)
		{
			const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
			float               v;
			if (e == 0) {
				v = std::ldexp(static_cast<float>(m), -24);
			} else if (e == 31) {
				v = m ? 0.0f : 65504.0f;
			} else {
				v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
			}
			return s ? -v : v;
		}

		// Copies a vertex buffer back from the graphics card and finds the bounds of its positions
		void Measure(ID3D11DeviceContext* c, const MeasureJob& a_job)
		{
			std::array<float, 6> out{ 1e9f, 1e9f, 1e9f, -1e9f, -1e9f, -1e9f };
			D3D11_BUFFER_DESC    bd{};
			a_job.vb->GetDesc(&bd);
			D3D11_BUFFER_DESC sd{};
			sd.ByteWidth = bd.ByteWidth;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ID3D11Buffer* staging = nullptr;
			const UINT    stride = static_cast<UINT>(a_job.desc & 0xF) * 4;
			if (stride > 0 && SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &staging)) && staging) {
				c->CopyResource(staging, a_job.vb);
				D3D11_MAPPED_SUBRESOURCE m{};
				if (SUCCEEDED(c->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
					const auto* p = static_cast<const std::uint8_t*>(m.pData);
					const bool  full = FullPrecision(a_job.desc);
					const UINT  off = AttrOffset(a_job.desc, 0);
					const UINT  n = (std::min)(a_job.count, bd.ByteWidth / stride);
					for (UINT k = 0; k < n; ++k) {
						const auto* v = p + static_cast<std::size_t>(k) * stride + off;
						float       xyz[3];
						if (full) {
							std::memcpy(xyz, v, sizeof(xyz));
						} else {
							std::uint16_t hv[3];
							std::memcpy(hv, v, sizeof(hv));
							for (int i = 0; i < 3; ++i) {
								xyz[i] = Half(hv[i]);
							}
						}
						for (int i = 0; i < 3; ++i) {
							out[i] = (std::min)(out[i], xyz[i]);
							out[3 + i] = (std::max)(out[3 + i], xyz[i]);
						}
					}
					c->Unmap(staging, 0);
				}
				staging->Release();
			}
			if (out[0] > out[3]) {
				out = { -1, -1, -1, 1, 1, 1 };  // couldn't read it: a unit box
			}
			std::lock_guard lk(g_measureMtx);
			auto&           e = g_measured[a_job.id];
			if (e.vb && e.vb != a_job.vb) {
				e.vb->Release();
			}
			if (e.vb == a_job.vb) {
				a_job.vb->Release();  // already held
			}
			e.box = out;
			e.vb = a_job.vb;  // the job's reference, kept
		}

		template <class T>
		void Upload(ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_buf, const T& a_data)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(a_ctx->Map(a_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				std::memcpy(m.pData, &a_data, sizeof(T));
				a_ctx->Unmap(a_buf, 0);
			}
		}

		// The game's own state, put back exactly afterwards (as Dear ImGui's DX11 backend does)
		struct Saved
		{
			UINT                      vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_VIEWPORT            vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_RECT                sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			ID3D11RasterizerState*    rs = nullptr;
			ID3D11BlendState*         bs = nullptr;
			FLOAT                     bf[4]{};
			UINT                      mask = 0;
			ID3D11DepthStencilState*  ds = nullptr;
			UINT                      ref = 0;
			ID3D11ShaderResourceView* srv[5]{};
			ID3D11SamplerState*       smp[3]{};
			ID3D11PixelShader*        ps = nullptr;
			ID3D11VertexShader*       vs = nullptr;
			ID3D11GeometryShader*     gs = nullptr;
			ID3D11Buffer*             vcb[4]{};
			ID3D11Buffer*             pcb[4]{};
			D3D11_PRIMITIVE_TOPOLOGY  topo{};
			ID3D11Buffer*             ib = nullptr;
			DXGI_FORMAT               ibFmt{};
			UINT                      ibOff = 0;
			ID3D11Buffer*             vb[2]{};
			UINT                      stride[2]{}, offset[2]{};
			ID3D11InputLayout*        il = nullptr;
			ID3D11RenderTargetView*   rtv = nullptr;
			ID3D11DepthStencilView*   dsv = nullptr;

			void Take(ID3D11DeviceContext* c)
			{
				c->RSGetViewports(&vpCount, vp);
				c->RSGetScissorRects(&scCount, sc);
				c->RSGetState(&rs);
				c->OMGetBlendState(&bs, bf, &mask);
				c->OMGetDepthStencilState(&ds, &ref);
				c->PSGetShaderResources(0, 5, srv);
				c->PSGetSamplers(0, 3, smp);
				c->PSGetShader(&ps, nullptr, nullptr);
				c->VSGetShader(&vs, nullptr, nullptr);
				c->GSGetShader(&gs, nullptr, nullptr);
				c->VSGetConstantBuffers(0, 4, vcb);
				c->PSGetConstantBuffers(0, 4, pcb);
				c->IAGetPrimitiveTopology(&topo);
				c->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
				c->IAGetVertexBuffers(0, 2, vb, stride, offset);
				c->IAGetInputLayout(&il);
				c->OMGetRenderTargets(1, &rtv, &dsv);
			}
			void Put(ID3D11DeviceContext* c)
			{
				c->RSSetViewports(vpCount, vp);
				c->RSSetScissorRects(scCount, sc);
				c->RSSetState(rs);
				c->OMSetBlendState(bs, bf, mask);
				c->OMSetDepthStencilState(ds, ref);
				c->PSSetShaderResources(0, 5, srv);
				c->PSSetSamplers(0, 3, smp);
				c->PSSetShader(ps, nullptr, 0);
				c->VSSetShader(vs, nullptr, 0);
				c->GSSetShader(gs, nullptr, 0);
				c->VSSetConstantBuffers(0, 4, vcb);
				c->PSSetConstantBuffers(0, 4, pcb);
				c->IASetPrimitiveTopology(topo);
				c->IASetIndexBuffer(ib, ibFmt, ibOff);
				c->IASetVertexBuffers(0, 2, vb, stride, offset);
				c->IASetInputLayout(il);
				c->OMSetRenderTargets(1, &rtv, dsv);
				Release(rs);
				Release(bs);
				Release(ds);
				for (auto*& s : srv) {
					Release(s);
				}
				for (auto*& s : smp) {
					Release(s);
				}
				Release(ps);
				Release(vs);
				Release(gs);
				for (auto*& b : vcb) {
					Release(b);
				}
				for (auto*& b : pcb) {
					Release(b);
				}
				Release(ib);
				for (auto*& b : vb) {
					Release(b);
				}
				Release(il);
				Release(rtv);
				Release(dsv);
			}
		};

		void FullScreen(ID3D11DeviceContext* c, ID3D11RenderTargetView* a_rtv, UINT w, UINT h, ID3D11PixelShader* a_ps, ID3D11ShaderResourceView* a_src,
			const float (&a_pass)[4], ID3D11BlendState* a_bs)
		{
			D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1 };
			c->RSSetViewports(1, &vp);
			c->OMSetRenderTargets(1, &a_rtv, nullptr);
			c->OMSetBlendState(a_bs, nullptr, 0xFFFFFFFF);
			Upload(c, g_cbPass, a_pass);
			c->PSSetConstantBuffers(2, 1, &g_cbPass);
			c->PSSetShader(a_ps, nullptr, 0);
			c->PSSetShaderResources(0, 1, &a_src);
			c->Draw(3, 0);
			ID3D11ShaderResourceView* none = nullptr;
			c->PSSetShaderResources(0, 1, &none);
		}

		// 4x4 helpers (row-major, clip = M * v)
		using M4 = std::array<float, 16>;
		M4 Mul(const M4& a, const M4& b)
		{
			M4 r{};
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					float s = 0;
					for (int k = 0; k < 4; ++k) {
						s += a[i * 4 + k] * b[k * 4 + j];
					}
					r[i * 4 + j] = s;
				}
			}
			return r;
		}
		M4 LookAt(const float e[3], const float t[3], const float up[3])
		{
			float f[3] = { t[0] - e[0], t[1] - e[1], t[2] - e[2] };
			const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
			for (auto& v : f) {
				v /= fl;
			}
			float r[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
			const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
			for (auto& v : r) {
				v /= rl;
			}
			const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
			auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
			return { r[0], r[1], r[2], -dot(r, e), u[0], u[1], u[2], -dot(u, e), -f[0], -f[1], -f[2], dot(f, e), 0, 0, 0, 1 };
		}
		M4 Ortho(float l, float r, float b, float t, float n, float f)
		{
			return { 2 / (r - l), 0, 0, -(r + l) / (r - l), 0, 2 / (t - b), 0, -(t + b) / (t - b), 0, 0, 1 / (n - f), n / (n - f), 0, 0, 0, 1 };
		}

		std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
	}

	void Submit(Frame&& a_frame)
	{
		std::lock_guard lk(g_mtx);
		g_frame = std::move(a_frame);
	}

	void Prewarm()
	{
		int expect = 0;
		if (!g_preState.compare_exchange_strong(expect, 1)) {
			return;
		}
		std::thread([] {
			static const std::pair<const char*, const char*> kAll[] = { { "VSMain", "vs_5_0" }, { "VSShadow", "vs_5_0" }, { "VSShadowCut", "vs_5_0" }, { "VSFull", "vs_5_0" },
				{ "VSSprite", "vs_5_0" }, { "PSMain", "ps_5_0" }, { "PSCut", "ps_5_0" }, { "PSSolid", "ps_5_0" }, { "PSParity", "ps_5_0" }, { "PSParityBack", "ps_5_0" },
				{ "PSShadowCut", "ps_5_0" }, { "PSSprite", "ps_5_0" }, { "PSDown", "ps_5_0" }, { "PSBlur", "ps_5_0" }, { "PSBack", "ps_5_0" }, { "PSComposite", "ps_5_0" } };
			for (const auto& [e, t] : kAll) {
				auto* b = CompileNow(e, t);
				std::lock_guard lk(g_preMtx);
				g_pre[e] = b;
			}
			g_preState.store(2);
		}).detach();
	}

	void Clear()
	{
		std::lock_guard lk(g_mtx);
		g_frame = Frame{};
	}

	void RequestMeasure(std::uint64_t a_id, ID3D11Buffer* a_vb, std::uint64_t a_desc, std::uint32_t a_count)
	{
		if (!a_vb) {
			return;
		}
		std::lock_guard lk(g_measureMtx);
		if (g_measured.contains(a_id)) {
			return;
		}
		for (const auto& j : g_measureJobs) {
			if (j.id == a_id) {
				return;
			}
		}
		a_vb->AddRef();  // kept alive until the render thread has read it
		g_measureJobs.push_back({ a_id, a_vb, a_desc, a_count });
	}

	void ForgetMeasures()
	{
		std::lock_guard lk(g_measureMtx);
		for (auto& [id, e] : g_measured) {
			if (e.vb) {
				e.vb->Release();
			}
		}
		g_measured.clear();
	}

	bool MeasureResult(std::uint64_t a_id, float (&a_min)[3], float (&a_max)[3])
	{
		std::lock_guard lk(g_measureMtx);
		const auto      it = g_measured.find(a_id);
		if (it == g_measured.end()) {
			return false;
		}
		for (int i = 0; i < 3; ++i) {
			a_min[i] = it->second.box[i];
			a_max[i] = it->second.box[3 + i];
		}
		return true;
	}

	namespace
	{
		ID3D11Texture2D* g_target = nullptr;  // the interface layer, when drawing into it (not held)
	}

	void SetTarget(ID3D11Texture2D* a_target) { g_target = a_target; }

	void Draw(IDXGISwapChain* a_sc, ID3D11Device* a_dev, ID3D11DeviceContext* c)
	{
		std::lock_guard lk(g_mtx);
		if (g_failed || (!a_sc && !g_target) || !a_dev || !c) {
			return;
		}
		if (!g_ready) {
			if (!g_frame.show) {
				return;
			}
			g_ready = Init(a_dev);
			if (!g_ready) {
				g_failed = true;
				spdlog::error("Kitchen renderer: couldn't start - the kitchen won't show");
				return;
			}
		}
		// the meshes waiting to be measured (a few a frame)
		{
			std::vector<MeasureJob> jobs;
			{
				std::lock_guard ml(g_measureMtx);
				const std::size_t n = (std::min<std::size_t>)(g_measureJobs.size(), 8);
				jobs.assign(g_measureJobs.begin(), g_measureJobs.begin() + n);
				g_measureJobs.erase(g_measureJobs.begin(), g_measureJobs.begin() + n);
			}
			for (const auto& j : jobs) {
				Measure(c, j);  // keeps the job's reference
			}
		}
		if (!g_frame.show) {
			return;
		}
		ID3D11Texture2D* back = nullptr;
		const bool       layer = g_target != nullptr;
		if (layer) {
			back = g_target;
			back->AddRef();
		} else if (FAILED(a_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
			return;
		}
		D3D11_TEXTURE2D_DESC bdsc{};
		back->GetDesc(&bdsc);
		if (!Targets(bdsc)) {
			back->Release();
			g_failed = true;
			return;
		}
		ID3D11RenderTargetView*       backRTV = nullptr;
		D3D11_RENDER_TARGET_VIEW_DESC rvd{};
		rvd.Format = Typed(bdsc.Format);
		rvd.ViewDimension = bdsc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
		a_dev->CreateRenderTargetView(back, &rvd, &backRTV);
		if (!backRTV) {
			back->Release();
			return;
		}
		Saved saved;
		saved.Take(c);

		const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_t0).count();
		c->VSSetShader(g_vsFull, nullptr, 0);
		c->GSSetShader(nullptr, nullptr, 0);
		c->IASetInputLayout(nullptr);
		c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		c->RSSetState(g_rsNoCull);
		c->OMSetDepthStencilState(g_dsOff, 0);
		ID3D11SamplerState* smp[3] = { g_smpWrap, g_smpShadow, g_smpClamp };
		c->PSSetSamplers(0, 3, smp);

		// 1. the world behind: blurred and darkened, as Skyrim's menus do
		if (layer) {
			// the interface layer holds no world to blur: the menus under ours are wiped and the world, composited
			// under the layer afterwards, darkened by it
			const float veil[4] = { 0.0f, 0.0f, 0.0f, (std::min)(0.85f, 0.35f + g_frame.blur) };
			c->ClearRenderTargetView(backRTV, veil);
		} else if (g_frame.blur > 0.001f && bdsc.SampleDesc.Count == 1) {
			// the world behind is frozen while the kitchen is open: blurred over its first few frames, then kept
			static ULONGLONG s_last = 0;
			const ULONGLONG  now = ::GetTickCount64();
			if (now - s_last > 400) {
				g_blurMade = 0;  // (opened again)
			}
			s_last = now;
			const UINT qw = (std::max)(1u, g_w / 4), qh = (std::max)(1u, g_h / 4);
			if (g_blurMade < 4) {
				++g_blurMade;
				c->CopyResource(g_bg, back);
				const float down[4] = { 1.0f / g_w, 1.0f / g_h, 0, 0 };
				FullScreen(c, g_qRTV[0], qw, qh, g_psDown, g_bgSRV, down, g_bsOpaque);
				for (int k = 0; k < 2; ++k) {
					const float h[4] = { 1.0f / qw, 1.0f / qh, 0, 0 }, v[4] = { 1.0f / qw, 1.0f / qh, 0, 1 };
					FullScreen(c, g_qRTV[1], qw, qh, g_psBlur, g_qSRV[0], h, g_bsOpaque);
					FullScreen(c, g_qRTV[0], qw, qh, g_psBlur, g_qSRV[1], v, g_bsOpaque);
				}
			}
			const float b[4] = { 0, 0, g_frame.blur, 0 };
			FullScreen(c, backRTV, g_w, g_h, g_psBack, g_qSRV[0], b, g_bsAlpha);
		}

		// 2. the frame's constants: our camera, the key light's shadow camera, the lights
		CBFrame fr{};
		std::memcpy(fr.viewProj, g_frame.viewProj, sizeof(fr.viewProj));
		{
			const float* kd = g_frame.keyDir;
			const float  kl = std::sqrt(kd[0] * kd[0] + kd[1] * kd[1] + kd[2] * kd[2]);
			const float  dir[3] = { kd[0] / kl, kd[1] / kl, kd[2] / kl };
			const float* sc = g_frame.shadowCenter;
			const float  lp[3] = { sc[0] + dir[0] * 60, sc[1] + dir[1] * 60, sc[2] + dir[2] * 60 };
			const float  up[3] = { 0, 1, 0 };
			const M4     lv = LookAt(lp, sc, up);
			const M4     lo = Ortho(-g_frame.shadowHalf[0], g_frame.shadowHalf[0], -g_frame.shadowHalf[1], g_frame.shadowHalf[1], 10, 120);
			const M4     lvp = Mul(lo, lv);
			std::memcpy(fr.lightViewProj, lvp.data(), sizeof(fr.lightViewProj));
			std::memcpy(fr.keyDir, dir, sizeof(dir));
		}
		auto lin = [](int hex, float k, float* out) {
			for (int i = 0; i < 3; ++i) {
				const float v = ((hex >> (16 - 8 * i)) & 0xFF) / 255.0f;
				out[i] = std::pow(v, 2.2f) * k;
			}
		};
		lin(0xffe0bc, 1.05f, fr.keyColor);
		lin(0xffd9a8, 0.36f, fr.sky);
		lin(0x1a120a, 0.36f, fr.ground);
		std::memcpy(fr.firePos, g_frame.fire, sizeof(fr.firePos));
		std::memcpy(fr.fireColor, g_frame.fireColor, sizeof(float) * 3);
		fr.fireColor[3] = g_frame.fireHeat;
		{
			const int n = (std::min)(static_cast<int>(g_frame.occluders.size()), 32);
			for (int k = 0; k < n; ++k) {
				std::memcpy(fr.occ[k], g_frame.occluders[k].data(), sizeof(float) * 4);
			}
			fr.occInfo[0] = static_cast<float>(n);
		}
		std::memcpy(fr.haze, g_frame.haze, sizeof(fr.haze));
		std::memcpy(fr.camPos, g_frame.camPos, sizeof(float) * 3);
		std::memcpy(fr.camRight, g_frame.camRight, sizeof(float) * 3);
		std::memcpy(fr.camUp, g_frame.camUp, sizeof(float) * 3);
		fr.params[0] = time;
		fr.params[1] = 1.0f / kShadow;
		fr.params[2] = g_frame.exposure;
		Upload(c, g_cbFrame, fr);
		ID3D11Buffer* cbs[2] = { g_cbFrame, g_cbObject };
		c->VSSetConstantBuffers(0, 2, cbs);
		c->PSSetConstantBuffers(0, 2, cbs);

		auto bindMesh = [&](const Item& it) -> bool {
			auto* il = Layout(it.desc);
			if (!il || !it.vb || !it.ib) {
				return false;
			}
			const UINT    stride = static_cast<UINT>(it.desc & 0xF) * 4;
			ID3D11Buffer* vbs[2] = { it.vb, g_dummyVB };
			UINT          strides[2] = { stride, 0 }, offs[2] = { 0, 0 };
			c->IASetInputLayout(il);
			c->IASetVertexBuffers(0, 2, vbs, strides, offs);
			c->IASetIndexBuffer(it.ib, DXGI_FORMAT_R16_UINT, 0);
			CBObject ob{};
			for (int k = 0; k < 3; ++k) {
				ob.rot0[k] = it.rot[k];
				ob.rot1[k] = it.rot[3 + k];
				ob.rot2[k] = it.rot[6 + k];
				ob.posScale[k] = it.pos[k];
				ob.scale3[k] = it.scale3[k] != 0.0f ? it.scale3[k] : 1.0f;
				ob.spec[k] = it.specColor[k] * it.specScale;
				ob.tint[k] = it.tint[k];
				ob.emissive[k] = it.emissive[k];
				ob.cookAxis[k] = it.cookAxis[k];
				ob.cookRef[k] = it.cookRef[k];
			}
			ob.posScale[3] = it.scale;
			ob.scale3[3] = it.fade ? 2.0f : it.blend ? 1.0f : 0.0f;
			ob.spec[3] = it.gloss;
			ob.misc[0] = it.envScale;
			ob.misc[1] = it.alpha;
			ob.misc[2] = it.alphaRef;
			ob.misc[3] = it.test ? 1.0f : 0.0f;
			ob.has[0] = it.diffuse != nullptr;
			ob.has[1] = it.normal != nullptr;
			ob.has[2] = it.env != nullptr;
			ob.has[3] = it.envMask != nullptr;
			std::memcpy(ob.lift, it.lift, sizeof(ob.lift));
			std::memcpy(ob.clips, it.clips, sizeof(ob.clips));
			std::memcpy(ob.flesh, it.flesh, sizeof(ob.flesh));
			std::memcpy(ob.shape, it.shape, sizeof(ob.shape));
			std::memcpy(ob.uvx, it.uv, sizeof(ob.uvx));
			std::memcpy(ob.cook, it.cook, sizeof(ob.cook));
			ob.cookAxis[3] = static_cast<float>(it.cookMode);
			ob.cookRef[3] = it.fleshTex ? 1.0f : 0.0f;
			ob.emissive[3] = it.sear ? 1.0f : 0.0f;
			ob.tint[3] = it.unshadowed ? 1.0f : 0.0f;
			std::memcpy(ob.hollow, it.hollow, sizeof(ob.hollow));
			std::memcpy(ob.fx, it.fx, sizeof(ob.fx));
			std::memcpy(ob.spot, it.spot, sizeof(ob.spot));
			std::memcpy(ob.fx2, it.fx2, sizeof(ob.fx2));
			ob.hollow2[0] = it.hollowDepth;
			Upload(c, g_cbObject, ob);
			return true;
		};

		// 3. the key light's shadows
		{
			c->OMSetRenderTargets(0, nullptr, g_shadowDSV);
			c->ClearDepthStencilView(g_shadowDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
			D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(kShadow), static_cast<float>(kShadow), 0, 1 };
			c->RSSetViewports(1, &vp);
			c->RSSetState(g_rsShadow);
			c->OMSetDepthStencilState(g_dsOn, 0);
			c->VSSetShader(g_vsShadow, nullptr, 0);
			c->PSSetShader(nullptr, nullptr, 0);
			for (const auto& it : g_frame.items) {
				if (!it.blend && it.shadow && bindMesh(it)) {
					const bool cut = HasClips(it);
					c->VSSetShader(cut ? g_vsShadowCut : g_vsShadow, nullptr, 0);
					c->PSSetShader(cut ? g_psShadowCut : nullptr, nullptr, 0);
					c->DrawIndexed(it.indexCount, 0, 0);
				}
			}
		}

		// 4. the kitchen, lit, into our multisampled HDR target; then the fire and the steam
		{
			const float clear[4] = { 0, 0, 0, 0 };
			c->ClearRenderTargetView(g_colorRTV, clear);
			c->ClearDepthStencilView(g_depthDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
			c->OMSetRenderTargets(1, &g_colorRTV, g_depthDSV);
			D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1 };
			c->RSSetViewports(1, &vp);
			c->RSSetState(g_rsNoCull);
			c->VSSetShader(g_vs, nullptr, 0);
			c->PSSetShader(g_ps, nullptr, 0);
			c->PSSetShaderResources(4, 1, &g_shadowSRV);
			auto drawPass = [&](bool a_blend) {
				c->OMSetBlendState(a_blend ? g_bsPremul : g_bsOpaque, nullptr, 0xFFFFFFFF);
				c->OMSetDepthStencilState(a_blend ? g_dsRead : g_dsOn, 0);
				for (const auto& it : g_frame.items) {
					if (it.blend != a_blend || it.flesh[3] > 3.5f || !bindMesh(it)) {
						continue;  // (a cut piece's face is drawn after, with its stencil)
					}
					ID3D11ShaderResourceView* t[4] = { it.diffuse, it.normal, it.env, it.envMask };
					c->PSSetShaderResources(0, 4, t);
					c->PSSetShaderResources(5, 1, &it.fleshTex);
					const float fm = it.flesh[3];  // 1-2 cut faces from the mesh's inside, 3 skin with flesh inside
					c->PSSetShader((fm > 0.5f && fm < 2.5f) ? g_psCut : g_ps, nullptr, 0);
					c->DrawIndexed(it.indexCount, 0, 0);
				}
			};
			drawPass(false);
			// the cut pieces' faces: for each, its skin's whole mesh counts surfaces into the stencil, then the face
			// is drawn on a box round it wherever the count was odd (inside the thing), clearing it as it goes
			for (const auto& h : g_frame.items) {
				if (h.flesh[3] <= 3.5f) {
					continue;
				}
				c->OMSetBlendState(g_bsNoColor, nullptr, 0xFFFFFFFF);
				bool any = false;
				for (int side = 0; side < 1; ++side) {  // (the count in front only: counting behind too lost faces)
					c->OMSetDepthStencilState(g_dsParity, 1);
					c->PSSetShader(g_psParity, nullptr, 0);
					for (const auto& sk : g_frame.items) {
						if (sk.flesh[3] < 2.5f || sk.flesh[3] > 3.5f || std::memcmp(sk.clips, h.clips, sizeof(h.clips)) != 0 || !bindMesh(sk)) {
							continue;
						}
						c->DrawIndexed(sk.indexCount, 0, 0);
						any = true;
					}
				}
				if (!any || !bindMesh(h)) {
					continue;
				}
				ID3D11ShaderResourceView* t[4] = { h.diffuse, nullptr, nullptr, nullptr };
				c->PSSetShaderResources(0, 4, t);
				c->PSSetShaderResources(5, 1, &h.fleshTex);
				c->OMSetBlendState(g_bsOpaque, nullptr, 0xFFFFFFFF);
				c->OMSetDepthStencilState(g_dsCap, 1);
				c->PSSetShader(g_psSolid, nullptr, 0);
				c->DrawIndexed(h.indexCount, 0, 0);
			}
			c->PSSetShader(g_ps, nullptr, 0);
			drawPass(true);
			// the sprites: soft ones first (steam, smoke, bubbles, salt), then the light (fire, embers, glow) on top
			if (!g_frame.sprites.empty()) {
				c->IASetInputLayout(nullptr);
				c->VSSetShader(g_vsSprite, nullptr, 0);
				c->PSSetShader(g_psSprite, nullptr, 0);
				c->VSSetConstantBuffers(3, 1, &g_cbSprite);
				c->PSSetConstantBuffers(3, 1, &g_cbSprite);
				c->OMSetDepthStencilState(g_dsRead, 0);
				for (int pass = 0; pass < 2; ++pass) {
					c->OMSetBlendState(pass == 0 ? g_bsPremul : g_bsAdd, nullptr, 0xFFFFFFFF);
					for (const auto& s : g_frame.sprites) {
						const bool additive = s.kind == Sprite::Kind::Flame || s.kind == Sprite::Kind::Ember || s.kind == Sprite::Kind::Glow;
						if (additive != (pass == 1)) {
							continue;
						}
						CBSprite sp{};
						std::memcpy(sp.pos, s.pos, sizeof(float) * 3);
						sp.pos[3] = s.size;
						std::memcpy(sp.color, s.color, sizeof(sp.color));
						sp.params[0] = static_cast<float>(static_cast<int>(s.kind));
						sp.params[1] = s.seed;
						sp.params[2] = s.rot;
						sp.params[3] = s.stretch;
						Upload(c, g_cbSprite, sp);
						c->Draw(6, 0);
					}
				}
			}
			ID3D11ShaderResourceView* none[6]{};
			c->PSSetShaderResources(0, 6, none);
			ID3D11RenderTargetView* noRT = nullptr;
			c->OMSetRenderTargets(1, &noRT, nullptr);
			if (g_msaa > 1) {
				c->ResolveSubresource(g_resolved, 0, g_color, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
			} else {
				c->CopyResource(g_resolved, g_color);
			}
		}

		// 5. onto the screen: tone mapped, faded in as it arrives
		{
			c->IASetInputLayout(nullptr);
			c->VSSetShader(g_vsFull, nullptr, 0);
			c->RSSetState(g_rsNoCull);
			c->OMSetDepthStencilState(g_dsOff, 0);
			const float p[4] = { 0, 0, g_frame.reveal, g_frame.exposure };
			FullScreen(c, backRTV, g_w, g_h, g_psComposite, g_resolvedSRV, p, g_bsPremul);
		}

		saved.Put(c);
		backRTV->Release();
		back->Release();
	}
}
