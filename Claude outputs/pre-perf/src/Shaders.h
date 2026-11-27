#pragma once
// GLSL 330 sources for the CoreRay renderer. Fragment shaders are assembled as
// HEADER + COMMON + body so lighting, shadows and fog are identical everywhere.

namespace shaders {

static const char* VS_STATIC = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPos;
out vec2 fragUV;
out vec3 fragNormal;
out vec4 fragColor;
out float fragVar;
void main() {
    fragPos = (matModel * vec4(vertexPosition, 1.0)).xyz;
    fragUV = vertexTexCoord;
    fragNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);
    fragColor = vertexColor;
    fragVar = 0.0;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

// Terrain: as VS_STATIC, plus the low-frequency fields computed per vertex on the CPU
static const char* VS_TERRAIN = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexTangent;
in vec2 vertexTexCoord2;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPos;
out vec2 fragUV;
out vec3 fragNormal;
out vec4 fragColor;
out float fragVar;
out vec4 fragField;
out float fragBand;
out float fragSand;
void main() {
    fragPos = (matModel * vec4(vertexPosition, 1.0)).xyz;
    fragUV = vertexTexCoord;
    fragNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);
    fragColor = vertexColor;
    fragVar = 0.0;
    fragField = vertexTangent;
    fragBand = vertexTexCoord2.x;
    fragSand = vertexTexCoord2.y;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

// Far trees: impostors. A camera-facing (around the vertical) quad per tree, showing the
// tree as baked from the two nearest of 8 directions around it, blended.
static const char* VS_IMPOSTOR = R"(#version 330
in vec3 vertexPosition;   // quad: x -0.5..0.5, y 0..1
layout(location = 10) in mat4 instanceTransform;
uniform mat4 mvp;
uniform vec3 camPos;
uniform float impSize;    // tile size (tree units)
uniform float impCy;      // tile centre height (tree units)
uniform float impViews;
out vec3 fragPos;
out vec2 fragUV;
out vec3 fragNormal;
out vec4 fragColor;
out float fragVar;
void main() {
    vec3 o = instanceTransform[3].xyz;
    vec3 c0 = instanceTransform[0].xyz, c1 = instanceTransform[1].xyz, c2 = instanceTransform[2].xyz;
    float sx = length(c0), sy = length(c1);
    vec3 f = camPos - o;
    f.y = 0.0;
    f = dot(f, f) > 1e-6 ? normalize(f) : vec3(0.0, 0.0, 1.0);
    vec3 R = vec3(f.z, 0.0, -f.x);
    // where the camera is, around the tree, in the tree's own frame
    float az = atan(dot(f, c0 / sx), dot(f, c2 / length(c2)));
    float t = mod(az / 6.2831853 * impViews + impViews, impViews);
    float k0 = floor(t);
    float k1 = mod(k0 + 1.0, impViews);
    vec3 wp = o + R * (vertexPosition.x * impSize * sx) + vec3(0.0, ((vertexPosition.y - 0.5) * impSize + impCy) * sy, 0.0);
    fragPos = wp;
    fragUV = vec2(vertexPosition.x + 0.5, vertexPosition.y);
    fragNormal = f;
    fragColor = vec4(k0, k1, t - k0, 0.0);
    fragVar = fract(sin(dot(o.xz, vec2(12.9898, 78.233))) * 43758.5453);
    gl_Position = mvp * vec4(wp, 1.0);
}
)";

// Instanced: trees (leaf cards + bark) & rocks. Branches sway with height, leaves flutter.
static const char* VS_INSTANCED = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
layout(location = 10) in mat4 instanceTransform;
uniform mat4 mvp;
uniform float time;
uniform float windAmount;
uniform float treeHeight;
out vec3 fragPos;
out vec2 fragUV;
out vec3 fragNormal;
out vec4 fragColor;
out float fragVar;
float h11(float n) { return fract(sin(n) * 43758.5453); }
void main() {
    vec3 origin = instanceTransform[3].xyz;
    float seed = origin.x * 0.137 + origin.z * 0.071;
    vec4 wp = instanceTransform * vec4(vertexPosition, 1.0);
    float hgt = max(vertexPosition.y, 0.0) / max(treeHeight, 1.0);
    float sway = sin(time * 1.1 + seed * 6.0) * 0.6 + sin(time * 2.3 + seed * 11.0) * 0.25;
    wp.xz += vec2(sway, sway * 0.6) * windAmount * hgt * hgt * 0.9;
    if (vertexColor.r < 0.25) {
        float f = sin(time * 7.0 + dot(vertexPosition, vec3(3.1, 2.3, 1.7)) + seed * 5.0);
        wp.xyz += vec3(f, f * 0.6, -f * 0.8) * 0.03 * windAmount * (0.4 + hgt);
    }
    fragPos = wp.xyz;
    fragUV = vertexTexCoord;
    fragNormal = normalize(mat3(instanceTransform) * vertexNormal);
    fragColor = vertexColor;
    fragVar = h11(seed);
    gl_Position = mvp * wp;
}
)";

static const char* VS_DEPTH = R"(#version 330
in vec3 vertexPosition;
uniform mat4 mvp;
void main() { gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";

static const char* VS_DEPTH_INSTANCED = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
layout(location = 10) in mat4 instanceTransform;
uniform mat4 mvp;
uniform float time;
uniform float windAmount;
uniform float treeHeight;
out vec2 fragUV;
out float fragPart;
void main() {
    vec3 origin = instanceTransform[3].xyz;
    float seed = origin.x * 0.137 + origin.z * 0.071;
    vec4 wp = instanceTransform * vec4(vertexPosition, 1.0);
    float hgt = max(vertexPosition.y, 0.0) / max(treeHeight, 1.0);
    float sway = sin(time * 1.1 + seed * 6.0) * 0.6 + sin(time * 2.3 + seed * 11.0) * 0.25;
    wp.xz += vec2(sway, sway * 0.6) * windAmount * hgt * hgt * 0.9;
    fragUV = vertexTexCoord;
    fragPart = vertexColor.r;
    gl_Position = mvp * wp;
}
)";

static const char* FS_DEPTH_ALPHA = R"(#version 330
in vec2 fragUV;
in float fragPart;
uniform sampler2D texture0;
out vec4 finalColor;
void main() {
    if (fragPart < 0.25 && texture(texture0, fragUV).a < 0.45) discard;
    finalColor = vec4(1.0);
}
)";

static const char* FS_DEPTH = R"(#version 330
out vec4 finalColor;
void main() { finalColor = vec4(1.0); }
)";

static const char* FS_HEADER = R"(#version 330
in vec3 fragPos;
in vec2 fragUV;
in vec3 fragNormal;
in vec4 fragColor;
in float fragVar;
out vec4 finalColor;
)";

static const char* FS_COMMON = R"(
uniform vec3 camPos;
uniform vec3 sunDir;
uniform vec3 sunColor;
uniform vec3 skyZenith;
uniform vec3 skyHorizon;
uniform vec3 groundColor;
uniform float ambient;
uniform float fogDensity;
uniform float time;
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform mat4 lightVP0;
uniform mat4 lightVP1;
uniform vec2 shadowTexel;     // 1/res for cascade 0 and 1 (same res)
uniform vec2 cascadeRange;    // world-space half extents of cascade 0 / 1
uniform vec3 cascadeCenter0;
uniform vec3 cascadeCenter1;
uniform int shadowsOn;
uniform int numLights;
uniform vec3 lightPos[16];
uniform vec3 lightCol[16];
uniform vec3 lightDir[16];
uniform vec2 lightCone[16];   // cos inner, cos outer (outer < -0.5 = point light)
uniform vec4 lightShape[16];  // xyz = beam's long axis (along the road), w = elongation (1 = round)
uniform sampler2D cloudMap;   // the clouds' shadow map (sunlight through the cloud layer)
uniform float cloudShadowAmt; // 0 = no cloud shadows (volumetrics off)
uniform float cloudCov;
uniform vec4 csmRect;         // x0, z0, 1/size (m), cloud base height the map is parametrised at
uniform float wetness;
uniform float snowCover;
uniform float flash;
uniform float night;
uniform float fogGrey;
// tunnels (generated worlds): a top-down map round the camera, 2 m texels, nearest texel per
// fetch: r = metres in from the nearest mouth, g = 1 inside a tunnel, b = road height there
uniform sampler2D tunnelMap;
uniform vec4 tunnelRect;      // x0, z0, 1/size (m), texels across (z = 0: no tunnels near)
uniform int tunnelSkip;       // the terrain: it is never seen from inside a tube
uniform int numTunnelLamps;
uniform vec3 tunnelLamp[24];  // the lamps' faces under the crown, nearest the camera first
uniform vec3 tunnelLampCol;
float gTun = 1.0;             // set by shade(): sky light reaching this fragment (fog reads it)

// x: sky visibility (1 in the open, falling off past the mouths), y: how far inside (0..1)
vec2 tunnelAt(vec3 wp) {
    if (tunnelRect.z <= 0.0 || tunnelSkip == 1) return vec2(1.0, 0.0);
    vec2 uv = (wp.xz - tunnelRect.xy) * tunnelRect.z;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) return vec2(1.0, 0.0);
    float N = tunnelRect.w;
    if (texelFetch(tunnelMap, ivec2(uv * N), 0).g < 0.5) return vec2(1.0, 0.0);   // most fragments stop here
    vec2 tc = uv * N - 0.5;
    ivec2 i0 = ivec2(floor(tc));
    vec2 f = fract(tc);
    float acc = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 q = clamp(i0 + o, ivec2(0), ivec2(int(N) - 1));
        vec4 t = texelFetch(tunnelMap, q, 0);
        // only under the vault: a road or the land well above the tube is in the open
        if (t.g < 0.5 || wp.y < t.b - 1.5 || wp.y > t.b + 6.75) continue;
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        acc += t.r * w;
        wsum += w;
    }
    if (wsum < 1e-3) return vec2(1.0, 0.0);
    float dm = acc / wsum;
    // daylight reaches a few metres in: half by ~4 m, a few percent by 20 m
    return vec2(exp(-max(dm, 0.0) / 6.0), clamp(wsum * 4.0, 0.0, 1.0));
}

// the tunnel's ceiling lamps: wide spots aimed down at the road, plus a little bounce off
// the lining so the far tube isn't black
vec3 tunnelLight(vec3 albedo, vec3 n, vec3 wp, vec3 V, float shin, float spec, float inside) {
    vec3 col = albedo * tunnelLampCol * 0.012 * inside;
    for (int i = 0; i < 24; i++) {
        if (i >= numTunnelLamps) break;
        vec3 l = tunnelLamp[i] - wp;
        float d2 = dot(l, l);
        if (d2 > 1024.0) continue;
        float d = sqrt(d2);
        vec3 ld = l / max(d, 1e-3);
        float win = clamp(1.0 - pow(d / 32.0, 4.0), 0.0, 1.0);
        float att = win * win / (1.0 + d2 * 0.22);
        float spot = smoothstep(-0.35, 0.45, -ld.y);                       // aimed down, spills on the walls
        float fade = 1.0 - smoothstep(210.0, 290.0, length(tunnelLamp[i] - camPos));   // the set is the nearest 24
        float nl = max(dot(n, ld) * 0.9 + 0.1, 0.0);
        vec3 hl = normalize(V + ld);
        col += tunnelLampCol * att * spot * fade * (albedo * nl + pow(max(dot(n, hl), 0.0), shin) * spec * nl * 0.8);
    }
    return col * inside;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + vec2(1, 0)), c = hash12(i + vec2(0, 1)), d = hash12(i + vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm3(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 3; i++) { s += vnoise(p) * a; p = mat2(1.6, 1.2, -1.2, 1.6) * p; a *= 0.5; }
    return s / 0.875;
}

const vec2 poisson[12] = vec2[](
    vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
    vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893),
    vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));

float shadowSample(sampler2D sm, mat4 vp, vec3 wp, float bias, float radius, int taps) {
    vec4 lp = vp * vec4(wp, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    if (p.x <= 0.001 || p.x >= 0.999 || p.y <= 0.001 || p.y >= 0.999 || p.z >= 1.0) return 1.0;
    float rot = hash12(gl_FragCoord.xy) * 6.2831;
    mat2 R = mat2(cos(rot), sin(rot), -sin(rot), cos(rot));
    float lit = 0.0;
    for (int i = 0; i < 12; i++) {
        if (i >= taps) break;
        float d = texture(sm, p.xy + R * poisson[i] * shadowTexel * radius).r;
        lit += (p.z - bias > d) ? 0.0 : 1.0;
    }
    return lit / float(taps);
}

// terrain sun shadows (World.cpp updateSunTerrain): the height a point must be above to see
// the sun past the ridges up-sun; 4 m of slack for the 20 m grid, a soft 4 m edge
uniform sampler2D sunTerrain;
uniform vec4 sunTerrainRect;  // x0, z0, 1/size (z = 0: none)
float terrainSun(vec3 wp) {
    if (sunTerrainRect.z <= 0.0) return 1.0;
    vec2 uv = (wp.xz - sunTerrainRect.xy) * sunTerrainRect.z;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 1.0;
    float S = textureLod(sunTerrain, uv, 0.0).r;
    return smoothstep(-2.0, 2.0, wp.y + 4.0 - S);
}
float shadowFactorMaps(vec3 wp, vec3 n);
float shadowFactor(vec3 wp, vec3 n) {
    float t = terrainSun(wp);
    return t <= 0.0 ? 0.0 : t * shadowFactorMaps(wp, n);
}
float shadowFactorMaps(vec3 wp, vec3 n) {
    if (shadowsOn == 0) return 1.0;
    float ndl = clamp(dot(n, sunDir), 0.0, 1.0);
    vec3 d = abs(wp - cascadeCenter0);
    vec3 d1 = abs(wp - cascadeCenter1);
    float r0 = cascadeRange.x * 0.92;
    // normal offset scaled by cascade texel size in world units
    float t0 = cascadeRange.x * 2.0 * shadowTexel.x;
    float t1 = cascadeRange.y * 2.0 * shadowTexel.x;
    float slope = sqrt(max(0.0, 1.0 - ndl * ndl)) / max(ndl, 0.15);
    if (max(d.x, d.z) < r0) {
        float s = shadowSample(shadowMap0, lightVP0, wp + n * t0 * 1.5, 0.0004 + 0.0006 * slope, 1.6, 12);
        float edge = smoothstep(r0 * 0.8, r0, max(d.x, d.z));
        if (edge > 0.0) s = mix(s, shadowSample(shadowMap1, lightVP1, wp + n * t1 * 1.5, 0.0006 + 0.001 * slope, 1.2, 5), edge);
        return s;
    }
    float r1 = cascadeRange.y * 0.95;
    float m = max(d1.x, d1.z);
    if (m < r1) {
        // the far cascade is soft anyway: fewer taps
        float s = shadowSample(shadowMap1, lightVP1, wp + n * t1 * 1.5, 0.0006 + 0.001 * slope, 1.2, 5);
        return mix(s, 1.0, smoothstep(r1 * 0.75, r1, m));
    }
    return 1.0;
}

// shadows of the volumetric clouds: where the light's ray meets the cloud layer, how covered is it
// soft shadows of the volumetric clouds: follow the light's ray up to the cloud base and read
// how much light the clouds let through there (ray-marched each frame, blurred)
float cloudShadow(vec3 wp) {
    if (cloudShadowAmt < 0.5) return 1.0;
    vec3 L = sunDir;
    L.y = max(L.y, 0.05);
    vec2 t = (wp.xz + L.xz / L.y * (csmRect.w - wp.y) - csmRect.xy) * csmRect.z;
    float far = 1.0 - 0.8 * cloudCov;   // beyond the map: the average
    if (t.x < 0.0 || t.y < 0.0 || t.x > 1.0 || t.y > 1.0) return far;
    float e = smoothstep(0.42, 0.5, max(abs(t.x - 0.5), abs(t.y - 0.5)));
    return mix(texture(cloudMap, t).r, far, e);
}

// street-light beam: a cone around lightDir squeezed across the road and stretched along it,
// brightest some way off vertical along the road (a "batwing" like a real road luminaire)
float spotShape(int i, vec3 ld) {
    vec3 r = -ld;
    vec3 D = lightDir[i];
    vec3 A = lightShape[i].xyz;
    float el = max(lightShape[i].w, 1.0);
    if (el <= 1.001) {
        float s = smoothstep(lightCone[i].y, lightCone[i].x, dot(r, D));
        return s * s;
    }
    vec3 B = normalize(cross(D, A));
    float al = dot(r, A), ac = dot(r, B), ad = dot(r, D);
    vec3 rr = normalize(vec3(al / el, ac, ad));
    float s = smoothstep(lightCone[i].y, lightCone[i].x, rr.z);
    return s * s * (1.0 + 0.7 * exp(-pow((abs(al) - 0.55) / 0.2, 2.0))) * smoothstep(-0.05, 0.12, ad);
}

vec3 skyColor(vec3 d) {
    float y = d.y;
    float sd = max(dot(d, sunDir), 0.0);
    vec3 c = mix(skyHorizon, skyZenith, pow(clamp(y, 0.0, 1.0), 0.45));
    c += sunColor * (pow(sd, 5.0) * 0.10 + pow(sd, 48.0) * 0.25);
    // horizon haze band
    c = mix(c, skyHorizon * 1.08 + sunColor * 0.03, exp(-abs(y) * 18.0) * 0.6);
    if (y < 0.0) c = mix(c, groundColor * 0.7 + skyHorizon * 0.3, smoothstep(0.0, -0.25, y));
    return c;
}

vec3 applyFog(vec3 col, vec3 wp) {
    vec3 v = wp - camPos;
    float dist = length(v);
    vec3 dir = v / max(dist, 1e-3);
    // height-attenuated exponential fog: valleys are hazier than ridges
    float hf = exp(-max(wp.y - 30.0, 0.0) * 0.0035);
    float f = 1.0 - exp(-pow(dist * fogDensity, 1.25) * (0.35 + 0.65 * hf));
    float sunAmt = pow(max(dot(dir, sunDir), 0.0), 8.0);
    vec3 fc = skyColor(normalize(vec3(dir.x, max(dir.y, 0.02), dir.z)));
    fc = mix(fc, fc + sunColor * 0.25, sunAmt);
    float g = dot(fc, vec3(0.333));
    fc = mix(fc, vec3(g) * vec3(0.96, 0.98, 1.02), fogGrey) + vec3(0.6, 0.65, 0.8) * flash * 0.25;
    fc *= gTun;   // the haze deep in a tunnel isn't lit by the sky
    return mix(col, fc, clamp(f, 0.0, 1.0));
}

// lanterns (spot lights pointing down) and the bike's headlight
vec3 localLight(vec3 albedo, vec3 n, vec3 wp, vec3 V, float shin, float spec) {
    vec3 col = vec3(0.0);
    for (int i = 0; i < 16; i++) {
        if (i >= numLights) break;
        vec3 l = lightPos[i] - wp;
        float d2 = dot(l, l);
        if (d2 > 3600.0) continue;
        float d = sqrt(d2);
        vec3 ld = l / max(d, 1e-3);
        // smooth windowed inverse-square falloff
        float win = clamp(1.0 - pow(d / 60.0, 4.0), 0.0, 1.0);
        float att = win * win / (1.0 + d2 * 0.18);
        float spot = lightCone[i].y > -0.5 ? spotShape(i, ld) : 1.0;
        float nl = max(dot(n, ld) * 0.9 + 0.1, 0.0);
        vec3 hl = normalize(V + ld);
        col += lightCol[i] * att * spot * (albedo * nl + pow(max(dot(n, hl), 0.0), shin) * spec * nl * 0.8);
    }
    return col;
}

// the grass ground, cartoon style: a clean painted green with broad, soft variation (lush,
// dry meadow, damp patches) and only a whisper of the blade texture up close. The grass
// tufts use the same colour at their roots, so they grow out of the ground seamlessly.
// the grass tufts' own colour (unchanged)
vec3 tuftGreen(vec2 xz) {
    float m1 = vnoise(xz * 0.0065), m2 = vnoise(xz * 0.021 + 7.0), m3 = vnoise(xz * 0.09 + 3.0);
    vec3 c = vec3(0.28, 0.43, 0.15);
    c *= 0.9 + 0.16 * m2;
    c = mix(c, vec3(0.52, 0.53, 0.22), smoothstep(0.58, 0.86, m1) * 0.55);   // dry meadow
    c = mix(c, c * vec3(0.80, 0.95, 0.85), smoothstep(0.62, 0.8, m3) * 0.3);  // damp patches
    return c;
}
// the ground's base green, Zwift-like: bright, saturated, warm, with broad soft swathes of
// lighter yellow-green and deeper green and no fine noise
vec3 meadowGreen(vec2 xz) {
    float m1 = vnoise(xz * 0.0045), m2 = vnoise(xz * 0.016 + 7.0), m3 = vnoise(xz * 0.05 + 3.0);
    vec3 c = vec3(0.31, 0.52, 0.14);
    c *= 0.94 + 0.1 * m2;
    c = mix(c, vec3(0.42, 0.57, 0.15), smoothstep(0.55, 0.85, m1) * 0.55);   // sunny, lighter swathes
    c = mix(c, vec3(0.23, 0.44, 0.13), smoothstep(0.6, 0.85, m3) * 0.45);    // deeper green
    return c;
}
vec3 grassAlbedo(sampler2D g, vec2 xz, out float h) {
    // the blade texture at half the frequency and blurred (mip bias): soft clumps, not grain
    vec4 a = texture(g, xz * 0.16, 1.5);
    float lum = dot(a.rgb, vec3(0.3, 0.55, 0.15));
    float mean = dot(textureLod(g, vec2(0.5), 12.0).rgb, vec3(0.3, 0.55, 0.15));
    float fadeDetail = 1.0 - smoothstep(6.0, 40.0, length(camPos.xz - xz));
    vec3 c = meadowGreen(xz) * (1.0 + clamp(lum - mean, -0.2, 0.2) * 0.12 * fadeDetail);   // a hint of blades, not speckle
    h = 0.35 + 0.3 * texture(g, xz * 0.16, 3.0).a;   // height for the bump: smooth
    return c;
}

// expanding raindrop rings: returns a normal tilt (xz)
vec2 ripples(vec2 p, float t) {
    vec2 tilt = vec2(0.0);
    for (int k = 0; k < 2; k++) {
        vec2 q = p * (k == 0 ? 2.2 : 3.7) + float(k) * 17.0;
        vec2 id = floor(q);
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                vec2 cell = id + vec2(i, j);
                float h = hash12(cell);
                vec2 c = cell + vec2(hash12(cell + 3.7), hash12(cell + 9.1));
                float life = fract(t * (0.9 + h * 0.6) + h * 7.0);
                vec2 dv = q - c;
                float d = length(dv);
                float r = life * 0.55;
                float w = sin((d - r) * 40.0) * exp(-pow((d - r) * 9.0, 2.0)) * (1.0 - life);
                tilt += dv / max(d, 1e-3) * w;
            }
    }
    return tilt * 0.18;
}

vec3 shade(vec3 albedo, vec3 n, vec3 wp, float rough, float spec, float shadow, float ao) {
    vec3 V = normalize(camPos - wp);
    float ndl = max(dot(n, sunDir), 0.0);
    vec3 H = normalize(V + sunDir);
    float shin = mix(300.0, 6.0, rough);
    float nh = max(dot(n, H), 0.0);
    float fres = 0.04 + 0.96 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
    float s = pow(nh, shin) * (shin + 8.0) / 25.13 * spec * mix(0.25, 1.0, fres);
    vec2 tun = tunnelAt(wp);   // in a tunnel the sky (and sun) only reach in from the mouths
    gTun = tun.x;
    vec3 hemi = mix(groundColor, skyZenith, n.y * 0.5 + 0.5);
    vec3 direct = sunColor * ndl * shadow * cloudShadow(wp) * tun.x;
    vec3 col = albedo * (direct + hemi * ambient * ao * tun.x);
    col += direct * s;
    col += skyColor(reflect(-V, n)) * fres * spec * 0.35 * ao * tun.x;
    // lightning lights everything from above for an instant
    col += albedo * vec3(0.75, 0.8, 1.0) * flash * (0.6 + 0.4 * max(n.y, 0.0)) * 1.6 * tun.x;
    col += localLight(albedo, n, wp, V, shin, spec);
    if (tun.y > 0.0) col += tunnelLight(albedo, n, wp, V, shin, spec, tun.y);
    return col;
}
)";

// ---------------------------------------------------------------- terrain
static const char* FS_TERRAIN = R"(
in vec4 fragField;   // per-vertex (CPU): geology region, moisture, 100 m patches, broad tone
in float fragBand;   // per-vertex: strata band offset
in float fragSand;   // generated worlds: loose sand at the surface
uniform int worldMode;        // 1: a generated world (fragField.w = desert tint, fragSand)
uniform int numPortals;       // tunnel portals: the hillside is open in front of them
uniform vec4 portalPos[12];   // xyz: the road at the hillside face, w: arch half width
uniform vec4 portalDir[12];   // xyz: into the tunnel (horizontal), w: arch crown height
uniform float waterLevel;
uniform float snowLine;
uniform float landBase;       // the route's lowest ground
uniform sampler2D texture0;   // grass
uniform sampler2D texture1;   // bedded rock
uniform sampler2D texture2;   // gravel
uniform sampler2D texSand;    // sand / silt
uniform sampler2D texSoil;    // bare earth
uniform sampler2D texGranite; // crystalline rock, jointed into blocks
uniform sampler2D texScree;   // angular talus
uniform sampler2D texMoor;    // heather / moss / bracken
uniform int debugMode;
// vertex colour: r = road shoulder, g = stream discharge, b = fresh sediment, a = curvature
// (0.5 flat, > concave hollows / valley floors / cliff feet, < convex ridges / crests)
// uv: x = scoured ground, y = shore (0.5+ = under water)

vec4 triplanar(sampler2D t, vec3 p, vec3 n, float sc) {
    vec3 w = pow(abs(n), vec3(4.0));
    w /= (w.x + w.y + w.z);
    // strata run horizontally on the cliff faces
    vec4 a = w.x > 0.02 ? texture(t, vec2(p.z, p.y) * sc) : vec4(0.0);
    vec4 b = w.y > 0.02 ? texture(t, vec2(p.x, p.z) * sc + 0.37) : vec4(0.0);
    vec4 c = w.z > 0.02 ? texture(t, vec2(p.x, p.y) * sc + 0.71) : vec4(0.0);
    return (a * w.x + b * w.y + c * w.z) / max(w.x * step(0.02, w.x) + w.y * step(0.02, w.y) + w.z * step(0.02, w.z), 1e-3);
}
// anti-tiling: a second, rotated and rescaled lookup blended in by a noise mask, with the
// variance restored around the texture's mean so the blend doesn't wash out
vec4 untile(sampler2D t, vec2 uv, bool far) {
    if (far) return texture(t, uv);   // past ~250 m the tiling can't be seen: one lookup
    float k = smoothstep(0.3, 0.7, vnoise(uv * 0.21));
    vec2 uv2 = mat2(0.8, -0.6, 0.6, 0.8) * uv * 0.87 + vec2(0.37, 0.61);
    vec4 a = texture(t, uv), b = texture(t, uv2);
    vec4 mean = textureLod(t, vec2(0.5), 12.0);
    float w = sqrt(k * k + (1.0 - k) * (1.0 - k));
    return clamp(mean + (mix(a, b, k) - mean) / w, 0.0, 1.0);
}
// height-aware blend: the taller texture pokes through at transitions
vec4 hblend(vec4 h, vec4 w) {
    vec4 t = h * 0.6 + w;
    float m = max(max(t.x, t.y), max(t.z, t.w)) - 0.22;
    vec4 b = max(t - m, 0.0) * step(0.001, w);
    return b / max(b.x + b.y + b.z + b.w, 1e-4);
}

void main() {
    for (int i = 0; i < 12; i++) {
        if (i >= numPortals) break;
        vec3 d = fragPos - portalPos[i].xyz;
        vec3 f = portalDir[i].xyz;
        float along = dot(d, f);
        float lat = abs(d.x * f.z - d.z * f.x);
        // cut to the tube's arch: vertical walls, then an elliptical vault 2.4 m tall
        float hw = portalPos[i].w, crown = portalDir[i].w, wall = crown - 2.4;
        float e = clamp(lat / hw, 0.0, 1.0);
        float top = wall + (crown - wall) * sqrt(1.0 - e * e);
        if (along > -2.5 && along < 18.0 && lat < hw && d.y > -1.0 && d.y < top) discard;
    }
    vec3 n0 = normalize(fragNormal);
    vec3 n = n0;
    vec2 xz = fragPos.xz;
    float camD = length(camPos - fragPos);
    float slope = 1.0 - n0.y;
    float h = fragPos.y;
    float shoulder = fragColor.r, stream = fragColor.g, sed = fragColor.b;
    float curv = (fragColor.a - 0.5) * 2.0;
    float scour = fragUV.x, shore = fragUV.y;
    float under = smoothstep(0.48, 0.6, shore);

    // ---- regional fields, domain-warped so boundaries flow with the land
    // (computed per vertex on the CPU: they change over hundreds of metres)
    float region = fragField.x;   // geology: bedded rock .. granite
    float moist = fragField.y;    // wetter .. drier country
    float patchA = fragField.z;   // ~100 m patches
    float arid = worldMode == 1 ? fragField.w : 0.0;      // desert tint (generated worlds)
    float tone = worldMode == 1 ? patchA : fragField.w;
    float sandCover = worldMode == 1 ? clamp(fragSand, 0.0, 1.0) : 0.0;
    bool far = camD > 250.0;
    float n1 = vnoise(xz * 0.06), n2 = vnoise(xz * 0.45), n3 = vnoise(xz * 1.1);
    float dn = n1 * 0.6 + n2 * 0.3 + n3 * 0.1;
    float alt = clamp((h - landBase) / max(snowLine - landBase, 400.0), 0.0, 1.2);   // valley floor 0 .. snow line 1
    float facing = dot(n0.xz, vec2(0.35, -0.94)) * 2.5;      // slopes turned to the midday sun dry out
    moist = clamp(moist + curv * 0.6 + stream * 0.5 - facing * 0.25 - alt * 0.15 + shore * 0.4, 0.0, 1.0);

    // ---- the green layer: meadow -> drier grass -> moorland -> alpine tundra
    float gh;
    vec3 grass = grassAlbedo(texture0, xz, gh);
    float dry = clamp(0.55 - moist + facing * 0.15 + arid * 0.35, 0.0, 1.0);
    grass = mix(grass, grass * vec3(1.35, 1.12, 0.62), smoothstep(0.1, 0.5, dry) * (0.55 + 0.4 * arid));     // straw (golden in dry country)
    grass = mix(grass, grass * vec3(0.82, 1.03, 0.86), smoothstep(0.55, 0.85, moist) * 0.5);  // lush hollows
    float moorW = smoothstep(0.42, 0.72, alt + (patchA - 0.5) * 0.45 - curv * 0.3) * (1.0 - shoulder);
    moorW = max(moorW, smoothstep(0.78, 0.92, patchA) * 0.55 * smoothstep(0.2, 0.4, alt));   // heathy patches lower down
    vec4 moor = moorW > 0.01 ? untile(texMoor, xz * 0.19, far) : vec4(0.0);
    vec3 green = mix(grass, moor.rgb, moorW);
    float tundra = smoothstep(0.78, 1.0, alt + (n1 - 0.5) * 0.2);
    green = mix(green, vec3(dot(green, vec3(0.35))) * vec3(1.02, 1.04, 0.9), tundra * 0.5);
    float greenH = mix(gh, moor.a, moorW);

    // ---- rock: two geologies, warped strata colour banding in the bedded one
    float geo = smoothstep(0.42, 0.6, region + (n2 - 0.5) * 0.08);
    vec4 rockA = geo < 0.99 ? triplanar(texture1, fragPos, n0, 0.11) : vec4(0.0);
    vec4 rockB = geo > 0.01 ? triplanar(texGranite, fragPos, n0, 0.075) : vec4(0.0);
    vec4 rock = mix(rockA, rockB, geo);
    float band = fract(h * 0.043 + fragBand);
    vec3 strata = mix(vec3(1.07, 0.98, 0.87), vec3(0.84, 0.83, 0.86), smoothstep(0.25, 0.75, band));
    strata = mix(strata, vec3(1.15, 0.88, 0.70), smoothstep(0.86, 0.96, band) * 0.7);   // ironstone
    rock.rgb *= mix(strata, vec3(1.0), geo);
    // desert country: red-brown strata, ochre soil
    rock.rgb = mix(rock.rgb, rock.rgb * vec3(1.28, 0.86, 0.66), arid * (1.0 - geo) * 0.85);
    rock.rgb = mix(rock.rgb, rock.rgb * vec3(0.82, 1.0, 0.68), smoothstep(0.55, 0.3, slope) * moist * 0.5);   // moss on the gentler rock

    // ---- loose ground: bare earth, and scree collecting under the cliffs
    vec4 soil = untile(texSoil, xz * 0.25, far);
    float screeAmt = smoothstep(0.12, 0.26, slope) * (1.0 - smoothstep(0.48, 0.62, slope))
                   * clamp(curv * 2.4 + (n2 - 0.5) * 0.7 + scour * 0.4 + alt * 0.4 - 0.05, 0.0, 1.0) * (1.0 - shoulder);
    vec4 scree = screeAmt > 0.01 ? untile(texScree, xz * 0.17, far) : vec4(0.0);
    vec4 loose = mix(soil, scree, screeAmt);
    loose.rgb = mix(loose.rgb, loose.rgb * vec3(1.2, 0.95, 0.75), arid * 0.6);

    vec4 grav = texture(texture2, xz * 0.55);
    vec4 sand = untile(texSand, xz * 0.21, far);
    // sediment: sand where it's slack, gravel bars by the current
    float gravBar = smoothstep(0.35, 0.7, vnoise(xz * 0.08) + stream * 0.4);
    vec4 sedT = mix(sand, grav * vec4(0.95, 0.95, 0.92, 1.0), gravBar * (1.0 - sandCover));
    sedT.rgb = mix(sedT.rgb, sedT.rgb * vec3(1.18, 1.02, 0.78), sandCover * 0.75);   // warm desert sand

    // layer weights: green | rock | loose | sediment
    float rockW = smoothstep(0.30, 0.52, slope + (dn - 0.5) * 0.22 - curv * 0.1) + scour * smoothstep(0.12, 0.3, slope) * 0.6;
    // convex crests shed their soil: outcrops on ridges even where gentle
    rockW = clamp(max(rockW, smoothstep(-0.3, -0.65, curv + (n2 - 0.5) * 0.35) * smoothstep(0.06, 0.18, slope) * 0.85), 0.0, 1.0);
    float soilW = smoothstep(0.24, 0.38, slope + (n2 - 0.5) * 0.15) * (1.0 - rockW) * 0.7 + smoothstep(0.3, 0.9, scour) * 0.75
                + pow(shoulder, 4.0) * (0.2 + 0.3 * dn) * 0.5 + smoothstep(0.55, 1.0, stream) * 0.35 + screeAmt * 0.9;
    // fresh, bare deposits by streams and shores; older ones on the slopes are grassed over
    float sedW = max(smoothstep(0.3, 1.0, sed) * (0.1 + 0.9 * clamp(max(smoothstep(0.5, 0.9, stream), shore * 2.0), 0.0, 1.0)), smoothstep(0.12, 0.5, shore + (dn - 0.5) * 0.15) * 0.95)
               + smoothstep(0.6, 0.95, stream) * 0.6;
    // generated worlds: dunes, washes and salt flats are sand; dry country shows its soil
    sedW = max(sedW, smoothstep(0.15, 0.7, sandCover + (n2 - 0.5) * 0.2));
    soilW = max(soilW, arid * smoothstep(0.35, 0.85, dry + (dn - 0.5) * 0.4) * 0.9);
    float grassW = max(1.0 - rockW - soilW * 0.7 - sedW, 0.0);
    vec4 W = hblend(vec4(greenH, rock.a, loose.a, sedT.a), vec4(grassW, rockW, soilW, sedW));
    vec3 albedo = green * W.x + rock.rgb * W.y + loose.rgb * W.z + sedT.rgb * W.w;
    float hgt = greenH * 0.015 * W.x + rock.a * 0.3 * W.y + loose.a * mix(0.06, 0.18, screeAmt) * W.z + sedT.a * 0.05 * W.w;
    // broad tonal variation so wide views don't flatten out
    albedo *= 0.9 + 0.2 * tone;

    // relief from the blended height (surface-gradient bump mapping), fading with distance
    float bumpAmt = (1.0 - smoothstep(20.0, 100.0, camD)) * 0.6;   // softer: fine relief read as noise
    if (bumpAmt > 0.0) {
        vec3 dpdx = dFdx(fragPos), dpdy = dFdy(fragPos);
        float dhdx = dFdx(hgt), dhdy = dFdy(hgt);
        vec3 r1 = cross(dpdy, n), r2 = cross(n, dpdx);
        float det = dot(dpdx, r1);
        if (abs(det) > 1e-8) n = normalize(n - (dhdx * r1 + dhdy * r2) / det * bumpAmt * 1.4);
    }
    // gentle macro detail normal
    float e = 0.35;
    float hx = vnoise((xz + vec2(e, 0)) * 0.45) - n2, hz = vnoise((xz + vec2(0, e)) * 0.45) - n2;
    n = normalize(n + vec3(-hx, 0.0, -hz) * 0.18 * (1.0 - W.y));   // gentle (it read as noise)

    vec3 snow = vec3(0.90, 0.93, 0.98);
    // snow lies in hollows first and is blown off the crests
    float snowAmt = smoothstep(snowLine, snowLine + 60.0, h + dn * 40.0 + curv * 30.0) * smoothstep(0.55, 0.25, slope);
    snowAmt = max(snowAmt, snowCover * smoothstep(0.62, 0.22, slope) * smoothstep(0.15, 0.55, snowCover + dn * 0.35 + curv * 0.25));
    snowAmt *= 1.0 - under;
    albedo = mix(albedo, snow, snowAmt);
    albedo *= albedo * 1.6;  // to linear-ish

    // wet: rain, damp banks, the beds of small streams; under water everything is soaked
    float damp = max(smoothstep(0.0, 0.5, shore) * 0.8, smoothstep(0.6, 0.95, stream) * 0.8);
    float wetT = max(wetness * (0.8 + 0.2 * clamp(curv * 2.0 + 0.5, 0.0, 1.0)), damp) * (1.0 - snowAmt);
    albedo *= mix(1.0, 0.55, wetT);
    albedo = mix(albedo, albedo * vec3(0.75, 0.85, 0.8), under);   // algae / silt film on the bed
    // small streams: a sheet of water over the bed, catching the sky
    float film = smoothstep(0.62, 0.95, stream) * (1.0 - under) * (1.0 - snowAmt);
    if (film > 0.0) {
        vec2 flowN = vec2(vnoise(xz * 1.3 + vec2(0.0, time * 0.9)), vnoise(xz * 1.3 + vec2(time * 0.7, 3.0))) - 0.5;
        n = normalize(mix(n, normalize(vec3(flowN.x * 0.25, 1.0, flowN.y * 0.25)), film));
    }

    float sh = shadowFactor(fragPos, n0);
    float ao = mix(0.72, 1.0, n1) * mix(0.8, 1.0, greenH * W.x + (1.0 - W.x)) * mix(0.85, 1.0, rock.a * W.y + (1.0 - W.y))
             * mix(1.0, 0.82, clamp(curv, 0.0, 1.0));   // hollows are a little occluded
    float rough = mix(mix(0.95, 0.6, snowAmt), 0.35, wetT);
    rough = mix(rough, 0.06, film);
    float spec = mix(0.04, 0.3, snowAmt) + wetT * 0.25 + film * 0.8 + W.y * geo * 0.06;   // granite crystals glint
    vec3 col = shade(albedo, n, fragPos, rough, spec, sh, ao);
    finalColor = vec4(applyFog(col, fragPos), 1.0);
    if (debugMode == 1) finalColor = vec4(albedo, 1.0);
    if (debugMode == 2) finalColor = vec4(n * 0.5 + 0.5, 1.0);
    if (debugMode == 3) finalColor = vec4(W.y, W.z, W.w, 1.0);
    if (debugMode == 4) finalColor = vec4(fragColor.rgb, 1.0);
    if (debugMode == 5) finalColor = vec4(fragUV, 0.0, 1.0);
    if (debugMode == 6) finalColor = vec4(vec3(rough, spec, film), 1.0);
    if (debugMode == 7) finalColor = vec4(fragColor.a, moorW, screeAmt, 1.0);
    if (debugMode == 8) finalColor = vec4(geo, moist, alt, 1.0);
    if (debugMode == 9) finalColor = vec4(vec3(cloudShadow(fragPos)), 1.0);
}
)";

// ---------------------------------------------------------------- road
static const char* FS_ROAD = R"(
uniform sampler2D texture0;   // asphalt (rgb, a = height)
uniform sampler2D texture1;   // gravel
uniform sampler2D texture2;   // grass
const float ROADW = 7.2;
void main() {
    vec3 n = normalize(fragNormal);
    float u = fragUV.x, v = fragUV.y;          // u: 0..1 across the asphalt, <0 / >1 on the verges; v: metres along
    float across = (u - 0.5) * ROADW;          // metres from the centre line
    vec3 V = normalize(camPos - fragPos);
    float camD = length(camPos - fragPos);

    // ---- where are we: asphalt, crumbling edge, gravel shoulder, grass
    float ragged = (vnoise(vec2(v * 1.1, 3.0)) - 0.5) * 0.22 + (vnoise(vec2(v * 6.0, 9.0)) - 0.5) * 0.10 + (vnoise(fragPos.xz * 9.0) - 0.5) * 0.05;
    float edgeDist = ROADW * 0.5 - abs(across) + ragged;        // >0 on the asphalt
    float onTar = smoothstep(-0.02, 0.03, edgeDist);
    float out1 = -edgeDist;                                     // metres beyond the edge
    float gravelW = 0.75 + (vnoise(vec2(v * 0.35, 1.0)) - 0.5) * 0.5;
    float grassMix = smoothstep(gravelW - 0.35, gravelW + 0.45, out1 + (vnoise(fragPos.xz * 2.3) - 0.5) * 0.5);
    grassMix = max(grassMix, smoothstep(2.2, 2.8, out1));      // always pure grass where the strip meets the terrain
    vec2 tunR = tunnelAt(fragPos);
    float dryR = clamp(tunR.x * 3.0, 0.0, 1.0);                // rain and snow reach a few metres in
    grassMix *= 1.0 - tunR.y;                                  // in a tunnel: gravel shoulders to the walls

    // ---- asphalt
    vec2 rp = vec2(across, v);
    // aggregate at a coarser scale and slightly blurred (mip bias): calm, not a pixel-level grain
    vec4 a1 = texture(texture0, rp / 4.6, 1.0);
    vec4 a2 = texture(texture0, rp / 13.0 + 0.31, 0.5);
    vec3 asph = mix(a1.rgb, a2.rgb, 0.3);
    // calm the aggregate: pull the texture halfway to its mean (detail fades out with distance too)
    vec3 aMean = textureLod(texture0, vec2(0.5), 12.0).rgb;
    asph = mix(aMean, asph, mix(0.22, 0.08, smoothstep(8.0, 40.0, camD)));
    float ah = a1.a * 0.7 + a2.a * 0.3;
    float macro = vnoise(rp * vec2(0.4, 0.08));
    asph *= 0.93 + 0.12 * macro;
    // repair patches: darker, smoother tar rectangles
    // repair patches: irregular, slightly darker and smoother tar, with a sealed seam
    float pn = vnoise(rp * vec2(0.9, 0.22) + 11.0) * 0.8 + vnoise(rp * vec2(3.0, 0.9)) * 0.2;
    float patchA = smoothstep(0.76, 0.775, pn);
    float seam = smoothstep(0.75, 0.76, pn) - smoothstep(0.775, 0.79, pn);
    asph = mix(asph, mix(asph, vec3(0.075, 0.075, 0.08), 0.65), patchA);
    asph = mix(asph, vec3(0.03), seam * 0.6);
    // cracks: thin dark meandering lines, mostly near the edges
    float cn = vnoise(rp * vec2(0.9, 0.35)) + 0.5 * vnoise(rp * vec2(2.4, 1.1));
    float crack = (1.0 - smoothstep(0.0, 0.025, abs(cn - 0.75))) * smoothstep(0.35, 0.75, vnoise(rp * 0.21 + 5.0));
    crack *= 0.35 + 0.65 * smoothstep(1.5, 3.4, abs(across));
    asph = mix(asph, vec3(0.04), crack * (1.0 - patchA) * 0.45);
    // tyre tracks: polished and a little darker, oil drips mid-lane
    float track = exp(-pow((abs(across) - 1.75) * 2.2, 2.0)) + 0.6 * exp(-pow((abs(across) - 0.75) * 2.6, 2.0));
    asph *= 1.0 - 0.12 * clamp(track, 0.0, 1.0);
    float oil = exp(-pow((abs(across) - 1.25) * 3.5, 2.0)) * smoothstep(0.55, 0.8, vnoise(vec2(across * 2.0, v * 0.6)));
    asph *= 1.0 - 0.25 * oil;

    // ---- paint: worn edge lines and centre dashes
    float aa = fwidth(across) * 1.5;
    float edgeLine = smoothstep(-aa, aa, abs(across) - (ROADW * 0.5 - 0.30)) - smoothstep(-aa, aa, abs(across) - (ROADW * 0.5 - 0.17));
    float dash = smoothstep(0.0, 0.05, fract(v / 9.0) - 0.45) * (1.0 - smoothstep(0.0, 0.05, fract(v / 9.0) - 0.93));
    float centre = (smoothstep(-aa, aa, across + 0.06) - smoothstep(-aa, aa, across - 0.06)) * dash;
    float wear = smoothstep(0.25, 0.7, a1.a + vnoise(rp * vec2(3.0, 0.7)) * 0.5);       // aggregate pokes through
    float paint = clamp(edgeLine + centre, 0.0, 1.0) * wear * (1.0 - track * 0.35);
    asph = mix(asph, vec3(0.80, 0.80, 0.76) * (0.85 + 0.15 * macro), paint);

    // ---- gravel shoulder + grass
    vec4 gv = texture(texture1, fragPos.xz / 2.2, 0.75);
    vec4 gv2 = texture(texture1, fragPos.xz / 6.5 + 0.5, 0.5);
    vec3 gMean = textureLod(texture1, vec2(0.5), 12.0).rgb;
    vec3 grav = mix(gMean, mix(gv.rgb, gv2.rgb, 0.3), 0.35) * (0.9 + 0.12 * vnoise(fragPos.xz * 0.5));
    grav = mix(grav, grav * vec3(0.55, 0.5, 0.42), smoothstep(0.1, 0.6, out1) * 0.35);  // dusty/damp away from the edge
    float gh;
    vec3 grass = grassAlbedo(texture2, fragPos.xz, gh);
    // stray grass growing through the gravel
    float tuft = smoothstep(0.6, 0.85, vnoise(fragPos.xz * 1.6)) * smoothstep(0.15, 0.9, out1);
    vec3 verge = mix(grav, grass, max(grassMix, tuft * 0.5 * (1.0 - tunR.y)));
    float vh = mix(gv.a, gh, grassMix);

    vec3 albedo = mix(verge, asph, onTar);
    float hgt = mix(vh, ah, onTar);

    // relief normal from the height maps (fades with distance)
    if (camD < 45.0) {
        // a soft relief from blurred heights over 6 cm (2 cm on full-res height read as grain)
        float k = (1.0 - smoothstep(8.0, 30.0, camD)) * 0.18;
        float hx = mix(texture(texture1, (fragPos.xz + vec2(0.06, 0.0)) / 2.2, 2.0).a, texture(texture0, (rp + vec2(0.06, 0.0)) / 4.6, 2.0).a, onTar);
        float hz = mix(texture(texture1, (fragPos.xz + vec2(0.0, 0.06)) / 2.2, 2.0).a, texture(texture0, (rp + vec2(0.0, 0.06)) / 4.6, 2.0).a, onTar);
        float hc = mix(texture(texture1, fragPos.xz / 2.2, 2.0).a, texture(texture0, rp / 4.6, 2.0).a, onTar);
        n = normalize(n + vec3(hc - hx, 0.0, hc - hz) * k * 2.0);
    }

    // ---- snow settles everywhere except where tyres run
    float sn = snowCover * dryR * mix(mix(0.95, 0.3, clamp(track, 0.0, 1.0)), 1.0, 1.0 - onTar) * (0.75 + 0.25 * hgt);
    albedo = mix(albedo, vec3(0.82, 0.85, 0.90), sn);
    albedo *= albedo * 1.6;

    // ---- wet: darker, glossy, puddles in the low spots with rain rings
    float wet = wetness * dryR * (1.0 - sn);
    float low = 1.0 - hgt;
    float puddle = smoothstep(0.62, 0.78, vnoise(fragPos.xz * 0.28) * 0.65 + low * 0.35 + vnoise(fragPos.xz * 1.9) * 0.15)
                   * wet * (1.0 - clamp(track, 0.0, 1.0) * 0.6) * mix(1.0, 0.6, 1.0 - onTar);
    albedo *= mix(1.0, mix(0.45, 0.55, 1.0 - onTar), wet);
    float rough = mix(mix(mix(0.82, 0.62, track), 0.55, patchA), 0.22, wet);
    rough = mix(rough, mix(0.95, 0.35, wet), (1.0 - onTar) * grassMix);
    rough = mix(rough, 0.9, (1.0 - onTar) * (1.0 - grassMix) * (1.0 - wet));
    float specA = mix(0.25 + track * 0.25, 0.9, wet);
    // the grass verge is shaded exactly like the terrain it runs into (no visible band)
    specA = mix(specA, 0.04 + wet * 0.25, (1.0 - onTar) * grassMix);
    float vergeAO = mix(0.72, 1.0, vnoise(fragPos.xz * 0.06)) * mix(0.8, 1.0, gh);
    float aoR = mix(1.0, vergeAO, (1.0 - onTar) * grassMix);
    if (wetness > 0.02 && camD < 60.0) {
        vec2 rip = ripples(fragPos.xz, time) * wetness * dryR * (0.35 + puddle);
        n = normalize(n + vec3(rip.x, 0.0, rip.y));
    }
    float sh = shadowFactor(fragPos, normalize(fragNormal));
    vec3 col = shade(albedo, n, fragPos, mix(rough, 0.03, puddle), specA + puddle * 1.6, sh, aoR);
    if (puddle > 0.0) {
        vec3 pn = n;
        float fr = 0.05 + 0.95 * pow(1.0 - max(dot(pn, V), 0.0), 5.0);
        col = mix(col, skyColor(reflect(-V, pn)) * 0.55, puddle * clamp(fr * 1.3, 0.0, 0.7));
    }
    // long glittering reflections of the lanterns on a wet road
    if (wet > 0.05) {
        vec3 R = reflect(-V, vec3(0.0, 1.0, 0.0));
        for (int i = 0; i < 16; i++) {
            if (i >= numLights) break;
            if (lightDir[i].y > -0.9) continue;   // lanterns only (not the headlight)
            vec3 tl = lightPos[i] - fragPos;
            float dl = length(tl);
            if (dl > 80.0) continue;
            vec3 dir = tl / dl;
            vec2 hR = normalize(R.xz + 1e-4), hL = normalize(dir.xz + 1e-4);
            float dh = 1.0 - dot(hR, hL);                  // sideways mismatch: tight
            float dv = R.y - dir.y;                        // vertical mismatch: smeared
            float k = exp(-dh * 6000.0 - dv * dv * 30.0);
            float shimmer = 0.6 + 0.4 * vnoise(fragPos.xz * vec2(6.0, 1.5) + time * 0.7);
            col += lightCol[i] * k * wet * 0.045 * shimmer * (0.5 + 0.5 * (1.0 - rough)) / (1.0 + dl * 0.08);
        }
    }
    finalColor = vec4(applyFog(col, fragPos), 1.0);
}
)";

// ---------------------------------------------------------------- generic objects
static const char* FS_OBJECT = R"(
uniform vec4 colDiffuse;
uniform float waterLine;
uniform float roughness;
uniform float specular;
uniform int pattern;      // 0 plain, 1 checker, 2 vertical stripes, 3 coursed stone (world space)
uniform float emissive;
void main() {
    vec3 n = normalize(fragNormal);
    if (!gl_FrontFacing) n = -n;
    vec3 albedo = colDiffuse.rgb * colDiffuse.rgb;
    if (pattern == 3) {
        // ashlar: courses of squared blocks, offset every row, mortar joints, weathering
        vec3 an = abs(n);
        vec2 q = an.x > an.z ? fragPos.zy : fragPos.xy;
        if (an.y > 0.7) q = fragPos.xz;
        q *= vec2(1.0 / 0.9, 1.0 / 0.38);
        float row = floor(q.y);
        q.x += hash12(vec2(row, 3.0)) * 0.7 + mod(row, 2.0) * 0.5;
        vec2 cell = floor(q), f = fract(q);
        float joint = min(min(f.x, 1.0 - f.x) * 0.9, min(f.y, 1.0 - f.y) * 0.38) / 0.04;
        float tone = hash12(cell + 7.0);
        albedo *= 0.78 + 0.34 * tone + 0.12 * (vnoise(fragPos.xz * 3.0 + fragPos.y) - 0.5);
        albedo = mix(albedo * 0.55, albedo, smoothstep(0.4, 1.0, joint));
        // damp, darker and greener towards the water
        float damp = 1.0 - smoothstep(0.0, 2.5, fragPos.y - waterLine);
        albedo = mix(albedo, albedo * vec3(0.55, 0.65, 0.5), damp * 0.8);
        if (an.y < 0.7) n = normalize(n + (vec3(vnoise(q * 5.0), 0.0, vnoise(q * 5.0 + 9.0)) - 0.5) * 0.25);
    }
    if (pattern == 1) {
        vec2 c = floor(fragUV * vec2(16.0, 2.0));
        albedo = mod(c.x + c.y, 2.0) < 1.0 ? vec3(0.02) : vec3(0.85);
    } else if (pattern == 2) {
        albedo *= 0.7 + 0.3 * step(0.5, fract(fragUV.x * 6.0));
    }
    float sh = shadowFactor(fragPos, n);
    vec3 col = shade(albedo, n, fragPos, roughness, specular, sh, 1.0) + albedo * emissive;
    finalColor = vec4(applyFog(col, fragPos), 1.0);
}
)";

// ---------------------------------------------------------------- vegetation (instanced)
static const char* FS_FOLIAGE = R"(
uniform int kind;    // 0 tree, 3 rock
uniform sampler2D texture0;   // leaf atlas
uniform int bakeMode;         // impostor baking: 1 = albedo, 2 = normal (camera basis) + ao
uniform vec3 bakeRight;
uniform vec3 bakeBack;
void main() {
    vec3 n = normalize(fragNormal);
    float v = fragVar;
    vec3 albedo;
    float rough = 0.85, spec = 0.06, ao = 1.0;
    bool leaf = false;
    vec3 V = normalize(camPos - fragPos);
    if (kind == 3) {
        // Zwift-style rock: clean warm grey facets with broad, soft colour shifts, darker where
        // it meets the ground, a cap of moss on the faces that look up
        vec3 base = mix(vec3(0.56, 0.54, 0.50), vec3(0.66, 0.60, 0.52), v);
        base *= 0.9 + 0.18 * vnoise(fragPos.xz * 0.35 + fragPos.y * 0.3 + v * 7.0);
        float mossy = smoothstep(0.78, 0.93, n.y + (vnoise(fragPos.xz * 1.4 + v * 3.0) - 0.5) * 0.25) * smoothstep(0.25, 0.6, v + 0.3);
        albedo = mix(base, vec3(0.40, 0.55, 0.18), mossy * 0.6);
        ao = mix(0.55, 1.0, fragColor.g);
        rough = 0.85; spec = 0.08;
    } else if (fragColor.r < 0.25) {
        leaf = true;
        vec4 t = texture(texture0, fragUV);
        // keep leaf coverage as the texture shrinks into its mip-maps
        vec2 dx = dFdx(fragUV * 1024.0), dy = dFdy(fragUV * 1024.0);
        float lod = max(0.0, 0.5 * log2(max(dot(dx, dx), dot(dy, dy))));
        if (t.a * (1.0 + lod * 0.32) < 0.5) discard;
        float hue = fragColor.b;
        // species tint (blue-green conifers .. warm yellow-green aspen), and tree to tree
        albedo = t.rgb * mix(vec3(0.88, 1.0, 0.95), vec3(1.08, 1.04, 0.8), hue) * (0.88 + 0.24 * v);
        albedo = mix(albedo, albedo * vec3(1.4, 0.95, 0.45), smoothstep(0.86, 1.0, v) * 0.45);   // the odd tree turning
        // darker under the canopy, as in a painted tree
        ao = fragColor.g * mix(0.75, 1.0, smoothstep(-0.6, 0.5, n.y));
        rough = 0.9; spec = 0.02;
    } else {
        // bark, per species: brown furrowed, birch white, pine orange plates, spruce grey scales
        float bt = fragColor.b;
        // painted bark: soft vertical streaks, little fine noise (the Zwift look)
        float furrow = vnoise(vec2(fragUV.x * 9.0, fragUV.y * 0.6));
        float detail = vnoise(vec2(fragUV.x * 24.0, fragUV.y * 3.0));
        vec3 brown = vec3(0.31, 0.26, 0.21) * (0.78 + 0.3 * furrow) * (0.94 + 0.1 * detail);
        albedo = brown;
        if (bt > 0.2 && bt < 0.5) {
            float marks = smoothstep(0.64, 0.72, vnoise(vec2(fragUV.x * 5.0, fragUV.y * 8.0)));
            albedo = mix(vec3(0.84, 0.82, 0.78) * (0.9 + 0.1 * detail), vec3(0.07, 0.06, 0.05), marks);
            albedo = mix(brown * 0.6, albedo, smoothstep(0.3, 1.4, fragUV.y));
        } else if (bt >= 0.5 && bt < 0.85) {
            float plates = smoothstep(0.4, 0.6, vnoise(vec2(fragUV.x * 9.0, fragUV.y * 2.5)));
            vec3 orange = vec3(0.66, 0.38, 0.21) * (0.8 + 0.25 * plates);
            albedo = mix(brown * 0.9, orange, smoothstep(5.0, 9.0, fragUV.y));
        } else if (bt >= 0.85) {
            albedo = vec3(0.30, 0.27, 0.24) * (0.6 + 0.45 * vnoise(vec2(fragUV.x * 22.0, fragUV.y * 4.0)));
        }
        n = normalize(n + (vec3(furrow, 0.0, detail) - 0.5) * 0.15);
        ao = fragColor.g;
    }
    albedo *= albedo * 1.5;
    if (bakeMode == 1) { finalColor = vec4(albedo * 0.6, 1.0); return; }
    if (bakeMode == 2) {
        vec3 nb = vec3(dot(n, bakeRight), n.y, dot(n, bakeBack));
        if (nb.z < 0.0) nb.xy = -nb.xy;   // leaf cards are two-sided: always face the viewer
        finalColor = vec4(nb.xy * 0.5 + 0.5, ao, 1.0);
        return;
    }
    float up = leaf ? n.y : normalize(fragNormal).y;
    float sn = snowCover * smoothstep(0.1, 0.6, up + (v - 0.5) * 0.3);
    albedo = mix(albedo * mix(1.0, 0.75, wetness), vec3(0.78, 0.81, 0.86), sn);
    float sh = shadowFactor(fragPos, normalize(fragNormal));
    vec3 col = shade(albedo, n, fragPos, rough, spec, sh, ao);
    if (leaf) {
        // a little light through the leaves when looking towards the sun (painted, not glowing)
        float trans = pow(max(dot(-V, sunDir), 0.0), 4.0) * 0.7 + 0.08;
        col += albedo * sunColor * trans * sh * ao * 0.35;
    }
    finalColor = vec4(applyFog(col, fragPos), 1.0);
}
)";

// ---------------------------------------------------------------- water
static const char* FS_WATER = R"(
// Rivers and lakes. The opaque scene (colour + depth) was copied just before this pass, so
// the surface can refract the real riverbed, absorb light by the actual path length through
// the water, light the bed with caustics, and reflect the banks (screen-space) or the sky.
// uv = surface current (m/s, world xz); colour r = distance to the edge (0 centre .. 1 shore), g = speed
// Drawn without depth testing: anything well above the surface (banks, bridges, the rider)
// hides it, but terrain only a hair above the waterline is covered, so shores follow the
// ribbon's smooth edge instead of the terrain grid.
uniform sampler2D refrColor;   // rgb = scene, a = linear depth
uniform vec2 screenSize;
uniform vec2 clipNF;          // near, far
uniform mat4 matView;
uniform mat4 matProjection;
uniform vec2 windVec;
uniform float turbidity;      // 0 clear .. 1 brown flood water

float linDepth(float d) {
    float z = d * 2.0 - 1.0;
    return 2.0 * clipNF.x * clipNF.y / (clipNF.y + clipNF.x - z * (clipNF.y - clipNF.x));
}
float sceneLin(vec2 uv) { return texture(refrColor, uv).a; }
// wave height field: a few octaves of moving noise
float waveH(vec2 p) {
    float h = vnoise(p * 0.55) * 0.5 + vnoise(p * 1.3 + 7.0) * 0.28 + vnoise(p * 3.1 + 3.0) * 0.14 + vnoise(p * 7.4) * 0.08;
    return h;
}
vec3 waveN(vec2 p, float amp) {
    float e = 0.08;
    float h0 = waveH(p), hx = waveH(p + vec2(e, 0.0)), hz = waveH(p + vec2(0.0, e));
    return normalize(vec3(-(hx - h0) / e * amp, 1.0, -(hz - h0) / e * amp));
}
float caustic(vec2 p, float t) {
    // two drifting warped cell patterns multiplied: bright networks like real caustics
    float c = 0.0;
    for (int i = 0; i < 2; i++) {
        vec2 q = p * (i == 0 ? 0.9 : 1.37) + vec2(t * 0.11, -t * 0.07) * (i == 0 ? 1.0 : -1.3);
        q += vec2(vnoise(q * 0.7 + t * 0.2), vnoise(q * 0.7 - t * 0.17 + 4.0)) * 1.4;
        vec2 f = fract(q) - 0.5;
        float d = 1.0 - smoothstep(0.0, 0.5, abs(length(f) - 0.32 - 0.05 * sin(t + q.x)));
        c += d;
    }
    return pow(c * 0.5, 2.2);
}

void main() {
    vec2 flow = fragUV;
    float speed = length(flow);
    vec2 p = fragPos.xz;
    float t = time;
    // flow mapping: two phases of ripples dragged along by the current, cross-faded
    vec2 drift = flow * 1.1 + windVec * (0.25 + 0.1 * (1.0 - min(speed, 1.0)));
    float ph0 = fract(t * 0.35), ph1 = fract(t * 0.35 + 0.5);
    float wgt = abs(ph0 - 0.5) * 2.0;
    float camDist = length(camPos - fragPos);
    float amp = (0.10 + 0.22 * min(speed, 2.0) + 0.06 * length(windVec)) / (1.0 + camDist / 150.0);
    vec3 nA = waveN(p - drift * ph0 * 2.6, amp), nB = waveN(p - drift * ph1 * 2.6 + 11.0, amp);
    vec3 n = normalize(mix(nA, nB, wgt));
    // long swell and fine wind ripples
    n = normalize(n + vec3(sin(p.x * 0.21 + t * 0.9) * 0.012, 0.0, cos(p.y * 0.17 - t * 0.8) * 0.012));
    if (wetness > 0.0) { vec2 rp = ripples(p * 1.4, t); n = normalize(n + vec3(rp.x, 0.0, rp.y) * wetness * 2.2); }

    vec3 V = normalize(camPos - fragPos);
    float NdV = max(dot(n, V), 0.0);
    float fres = 0.02 + 0.98 * pow(1.0 - NdV, 5.0);

    // ---------------- refraction: what's under the surface
    vec2 suv = gl_FragCoord.xy / screenSize;
    float surfLin = linDepth(gl_FragCoord.z);
    vec3 camFwd = -vec3(matView[0][2], matView[1][2], matView[2][2]);
    vec3 rayDir = -V;
    float cosA = max(dot(rayDir, camFwd), 0.05);
    {   // manual occlusion
        float sl = sceneLin(suv);
        if (sl < surfLin - 0.01) {
            float along = (surfLin - sl) / cosA;          // how far in front of the water it is
            vec3 sp = fragPos - rayDir * along;
            // ground only a hair above the waterline right at the shore is covered; anything
            // standing up, or anything more than a few metres in front (trees, the far bank
            // seen past a hill), hides the water
            if (sp.y > fragPos.y + 0.35 || along > 3.0 + 6.0 * (1.0 - cosA)) discard;
        }
    }
    float edgeA = 1.0 - smoothstep(0.78, 1.0, fragColor.r);
    float bedLin0 = sceneLin(suv);
    float thick0 = max(bedLin0 - surfLin, 0.0);
    vec2 off = n.xz * 0.05 * clamp(thick0 * 0.6, 0.0, 1.0) / max(surfLin * 0.02, 1.0);
    vec2 ruv = clamp(suv + off, vec2(0.001), vec2(0.999));
    float bedLin = sceneLin(ruv);
    if (bedLin < surfLin) { ruv = suv; bedLin = sceneLin(suv); }  // something in front: don't refract it
    float path = max(bedLin - surfLin, 0.0) / cosA;              // metres travelled through water
    vec3 bedPos = fragPos + rayDir * path;
    float vDepth = max(fragPos.y - bedPos.y, 0.0);               // vertical depth at the bed
    vec3 bed = texture(refrColor, ruv).rgb;

    // caustics on the bed (sunlit, fading with depth)
    float sunUp = max(sunDir.y, 0.0);
    float bedSh = shadowFactor(bedPos, vec3(0.0, 1.0, 0.0));
    vec2 cp = bedPos.xz + sunDir.xz / max(sunDir.y, 0.2) * vDepth * 0.3;
    float cst = caustic(cp * 0.9, t) * exp(-vDepth * 0.35) * smoothstep(0.02, 0.4, vDepth);
    bed += bed * sunColor * cst * 1.6 * sunUp * bedSh * (1.0 - clamp(turbidity + 0.1 * min(speed, 1.5), 0.0, 1.0) * 0.7);

    // absorption + in-scattering (Beer-Lambert), turbid water scatters more and absorbs blue
    float turb = clamp(turbidity + 0.1 * min(speed, 1.5), 0.0, 1.0);
    vec3 absorb = mix(vec3(0.42, 0.075, 0.055), vec3(0.75, 0.55, 0.62), turb);
    vec3 scatterCol = mix(vec3(0.02, 0.075, 0.07), vec3(0.16, 0.13, 0.07), turb);
    float density = 0.8 + turb * 2.2;
    vec3 T = exp(-absorb * path * density);
    vec3 skyAmb = mix(groundColor, skyZenith, 0.7) * ambient * 1.4;
    vec3 inscatter = scatterCol * (sunColor * (0.25 + 0.75 * sunUp) * 0.35 + skyAmb);
    vec3 refr = bed * T + inscatter * (1.0 - T);

    // ---------------- reflection: march the reflected ray against the depth copy
    vec3 R = reflect(-V, n);
    if (R.y < 0.0) R.y = -R.y * 0.3;
    vec3 refl = skyColor(normalize(R));
    float ssrHit = 0.0;
    {
        vec3 pos = fragPos;
        float stepLen = 0.6 + surfLin * 0.01;
        for (int i = 0; i < 28; i++) {
            pos += R * stepLen;
            stepLen *= 1.18;
            vec4 clip = matProjection * matView * vec4(pos, 1.0);
            if (clip.w <= 0.0) break;
            vec3 ndc = clip.xyz / clip.w;
            vec2 uv = ndc.xy * 0.5 + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
            float sl = sceneLin(uv);
            float rayLin = clip.w;
            if (rayLin > sl + 0.05 && rayLin - sl < stepLen * 2.5 + 1.0) {
                vec2 e = min(uv, 1.0 - uv);
                float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
                refl = mix(refl, texture(refrColor, uv).rgb, edge * smoothstep(1.0, 0.6, float(i) / 28.0));
                ssrHit = edge;
                break;
            }
        }
    }

    vec3 col = mix(refr, refl, fres);
    // sun glint
    float sh = shadowFactor(fragPos, vec3(0.0, 1.0, 0.0));
    vec3 H = normalize(V + sunDir);
    float rough = 0.03 + 0.05 * min(speed, 2.0) + wetness * 0.05;
    float a2 = rough * rough;
    float nh = max(dot(n, H), 0.0);
    float dd = nh * nh * (a2 - 1.0) + 1.0;
    float ggx = a2 / (3.14159 * dd * dd);
    col += sunColor * ggx * fres * 0.9 * sh * step(0.0, sunDir.y);
    col += localLight(vec3(0.0), n, fragPos, V, 400.0, 1.6) * 0.6;

    // foam: shallow edges and fast water, streaked along the current
    float edgeFoam = max(smoothstep(0.7, 0.93, fragColor.r) * (1.0 - smoothstep(0.93, 1.0, fragColor.r)) * step(0.01, speed),
                         (1.0 - smoothstep(0.0, 0.25 + 0.2 * min(speed, 2.0), path)) * 0.6);
    vec2 fp = p - flow * t * 0.9;
    float fn = vnoise(fp * vec2(1.6, 1.6) + vnoise(fp * 0.4) * 2.0);
    float rapid = smoothstep(1.7, 2.6, speed) * smoothstep(0.6, 0.85, fn) * 0.7;
    float foam = clamp(edgeFoam * smoothstep(0.3, 0.7, fn + 0.25) + rapid * 0.6, 0.0, 1.0);
    vec3 foamCol = vec3(0.85, 0.88, 0.9) * (sunColor * max(sunDir.y, 0.0) * sh * 0.5 + skyAmb * 1.2);
    col = mix(col, foamCol, foam * 0.75);
    // the waterline itself: fade in over the first few centimetres so shores don't show an edge
    vec3 behind = texture(refrColor, suv).rgb;
    finalColor = vec4(mix(behind, applyFog(col, fragPos), edgeA), 1.0);
}
)";

// ---------------------------------------------------------------- sky dome
static const char* FS_SKY = R"(
uniform float cloudCover;
uniform float cloudDark;
uniform vec3 realSunDir;
uniform vec3 moonDir;
uniform float moonIllum;
uniform float moonWaxing;
uniform float starAmt;
uniform float rainbow;
uniform float sunDisk;
uniform float volClouds;      // 1 = the volumetric pass draws the clouds
float starField(vec3 d) {
    // stars on a cube-ish grid of directions, twinkling a little
    vec3 a = abs(d);
    vec2 uv = a.x > a.y && a.x > a.z ? d.yz / a.x : a.y > a.z ? d.xz / a.y : d.xy / a.z;
    float face = a.x > a.y && a.x > a.z ? sign(d.x) : a.y > a.z ? 2.0 * sign(d.y) : 3.0 * sign(d.z);
    vec2 g = uv * 180.0;
    vec2 id = floor(g);
    float h = hash12(id + face * 17.0);
    if (h < 0.985) return 0.0;
    vec2 p = fract(g) - 0.5 - (vec2(hash12(id + 3.1), hash12(id + 7.7)) - 0.5) * 0.6;
    float b = (h - 0.985) / 0.015;
    float tw = 0.75 + 0.25 * sin(time * (2.0 + h * 5.0) + h * 40.0);
    return smoothstep(0.12, 0.0, length(p)) * b * b * tw;
}
void main() {
    vec3 d = normalize(fragPos - camPos);
    vec3 c = skyColor(d);
    float sd = dot(d, realSunDir);
    // stars and the moon sit behind the clouds
    if (d.y > 0.0) {
        float st = starField(d) * starAmt * smoothstep(0.0, 0.25, d.y);
        c += vec3(0.85, 0.9, 1.0) * st * 3.0;
        // milky band
        float band = exp(-pow(dot(d, normalize(vec3(0.3, 0.2, 0.93))) * 3.2, 2.0)) * fbm3(d.xz * 6.0 / (d.y + 0.3));
        c += vec3(0.05, 0.06, 0.09) * band * starAmt;
    }
    float md = dot(d, moonDir);
    if (md > 0.9985 && moonDir.y > -0.05) {
        // disk coordinates: x across (towards the sun side), y up
        vec3 right = normalize(cross(moonDir, vec3(0.0, 1.0, 0.0)));
        vec3 upv = cross(right, moonDir);
        vec2 q = vec2(dot(d - moonDir, right), dot(d - moonDir, upv)) / 0.0547;
        float r = length(q);
        if (r < 1.0) {
            float z = sqrt(max(0.0, 1.0 - r * r));
            float side = moonWaxing > 0.5 ? -1.0 : 1.0;
            // terminator: lit where the disk's x passes the phase threshold
            float k = moonIllum * 2.0 - 1.0;
            float lit = smoothstep(-0.08, 0.08, side * q.x + k * z);
            float maria = 0.75 + 0.25 * fbm3(q * 2.4 + 4.0);
            vec3 mc = vec3(1.0, 0.97, 0.9) * maria * (0.04 + 1.4 * lit) * (1.0 + 2.0 * night);
            c = mix(c, mc, smoothstep(1.0, 0.95, r));
        }
    }
    c += vec3(0.7, 0.75, 0.9) * pow(max(md, 0.0), 300.0) * 0.4 * moonIllum * night;
    // the sun's disk (the volumetric clouds cover it in their own pass)
    if (volClouds > 0.5) c += sunColor * smoothstep(0.99955, 0.99975, sd) * 30.0 * sunDisk;
    // clouds on a virtual plane (when the volumetric pass is off)
    if (d.y > 0.0 && volClouds < 0.5) {
        vec2 p = d.xz / (d.y + 0.08) * 1400.0 + camPos.xz * 0.3 + vec2(time * 6.0, time * 2.0);
        vec2 q = p * 0.00055;
        float n = fbm3(q) * 0.65 + fbm3(q * 3.1 + 5.0) * 0.35;
        float cov = smoothstep(1.0 - cloudCover, 1.0 - cloudCover + 0.3, n);
        cov = max(cov, smoothstep(0.75, 1.0, cloudCover) * 0.9);
        float ns = fbm3((p + sunDir.xz * 120.0) * 0.00055) * 0.65 + fbm3((p + sunDir.xz * 120.0) * 0.00055 * 3.1 + 5.0) * 0.35;
        float selfShadow = clamp((n - ns) * 2.5 + 0.6, 0.25, 1.0);
        vec3 cloudCol = mix(skyHorizon * 0.7 + vec3(0.12), sunColor * 0.55 + skyZenith * 0.2, selfShadow);
        cloudCol += sunColor * pow(max(sd, 0.0), 12.0) * 0.6 * (1.0 - cov * 0.5);
        cloudCol *= 1.0 - cloudDark * 0.65;
        cloudCol += vec3(0.8, 0.85, 1.0) * flash * 1.5;
        float fade = smoothstep(0.0, 0.18, d.y);
        c = mix(c, cloudCol, cov * fade * 0.92);
        // sun disk only where the clouds are thin
        c += sunColor * smoothstep(0.99955, 0.99975, sd) * 30.0 * sunDisk * (1.0 - cov * 0.92);
    }
    // rainbow: a 42 degree ring around the antisolar point
    if (rainbow > 0.001 && d.y > -0.02) {
        float ang = degrees(acos(clamp(dot(d, -realSunDir), -1.0, 1.0)));
        float t = (ang - 40.4) / 2.2;
        if (t > 0.0 && t < 1.0) {
            vec3 spec = clamp(vec3(abs(t * 6.0 - 3.0) - 1.0, 2.0 - abs(t * 6.0 - 2.0), 2.0 - abs(t * 6.0 - 4.0)), 0.0, 1.0);
            float a = sin(t * 3.14159) * rainbow * 0.35 * smoothstep(-0.02, 0.15, d.y);
            c += spec.zyx * a;
        }
    }
    finalColor = vec4(c, 1.0);
}
)";


// ---------------------------------------------------------------- tree impostors
static const char* FS_IMPOSTOR = R"(
uniform sampler2D texture0;   // albedo atlas (premultiplied by coverage)
uniform sampler2D texture1;   // normal atlas: rg = normal xy (camera basis), b = ao, a = coverage
uniform vec2 impGrid;         // views, types
uniform float impRow;
uniform int impDebug;
void main() {
    if (impDebug == 1) { finalColor = vec4(1.0, 0.0, 0.0, 1.0); return; }
    float k0 = fragColor.r, k1 = fragColor.g, w = fragColor.b;
    vec2 a0 = (vec2(k0, impRow) + fragUV) / impGrid, a1 = (vec2(k1, impRow) + fragUV) / impGrid;
    vec4 c0 = texture(texture0, a0), c1 = texture(texture0, a1);
    float cov = mix(c0.a, c1.a, w);
    if (cov < 0.42) discard;
    vec3 albedo = (c0.rgb * (1.0 - w) + c1.rgb * w) / max(cov, 1e-3) / 0.6;
    vec4 n0 = texture(texture1, a0), n1 = texture(texture1, a1);
    vec4 nn = (n0 * (1.0 - w) + n1 * w) / max(mix(n0.a, n1.a, w), 1e-3);
    vec2 nxy = nn.rg * 2.0 - 1.0;
    float ao = nn.b;
    vec3 B = normalize(fragNormal);
    vec3 R = vec3(B.z, 0.0, -B.x);
    vec3 n = normalize(R * nxy.x + vec3(0.0, 1.0, 0.0) * nxy.y + B * sqrt(max(1.0 - dot(nxy, nxy), 0.0)));
    albedo *= 0.82 + 0.32 * fragVar;
    float sn = snowCover * smoothstep(0.1, 0.6, n.y + (fragVar - 0.5) * 0.3);
    albedo = mix(albedo * mix(1.0, 0.75, wetness), vec3(0.78, 0.81, 0.86), sn);
    float sh = shadowFactor(fragPos, n);
    vec3 col = shade(albedo, n, fragPos, 0.72, 0.1, sh, ao);
    vec3 V = normalize(camPos - fragPos);
    col += albedo * sunColor * (pow(max(dot(-V, sunDir), 0.0), 4.0) * 0.9 + 0.15) * sh * ao * 0.5;
    finalColor = vec4(applyFog(col, fragPos), 1.0);
}
)";

// ---------------------------------------------------------------- instanced grass tufts
static const char* VS_GRASS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
layout(location = 10) in mat4 instanceTransform;
uniform mat4 mvp;
uniform float time;
uniform vec2 windVec;
uniform float windAmount;
uniform float gust;
uniform float snowFlat;
uniform vec3 riderPos;
out vec3 fragPos;
out vec2 fragUV;
out vec3 fragNormal;
out vec4 fragColor;
out float fragVar;
out vec3 fragRoot;
void main() {
    vec3 origin = instanceTransform[3].xyz;
    float h = clamp(vertexPosition.y, 0.0, 1.0);
    vec3 lp = vec3(vertexPosition.x, vertexPosition.y * (1.0 - snowFlat * 0.8), vertexPosition.z);
    vec4 wp = instanceTransform * vec4(lp, 1.0);
    float height = length(instanceTransform[1].xyz);
    float along = dot(origin.xz, windVec);
    // a travelling gust band rolls across the field, plus per-tuft flutter
    float band = 0.5 + 0.5 * sin(along * 0.11 - time * 1.7);
    band = band * band * band;
    float flutter = sin(time * 2.6 + origin.x * 0.9 + origin.z * 0.7) * 0.5 + sin(time * 4.3 + origin.z * 2.3) * 0.25;
    float sway = windAmount * (0.55 + 0.45 * flutter) + band * (0.35 + gust) * windAmount * 1.4 + flutter * 0.18;
    float bend = h * h;
    vec2 side = vec2(-windVec.y, windVec.x);
    vec2 push = windVec * sway + side * sin(time * 3.1 + origin.z * 1.7) * 0.15 * (0.3 + windAmount);
    // brushed aside by the bike
    vec2 away = origin.xz - riderPos.xz;
    float dr = length(away);
    push += away / max(dr, 0.1) * (1.0 - smoothstep(0.6, 1.6, dr)) * 1.2;
    wp.xz += push * bend * height * 0.5;
    wp.y -= bend * height * 0.18 * min(1.0, length(push));
    fragPos = wp.xyz;
    fragUV = vertexTexCoord;
    vec3 n = normalize(mat3(instanceTransform) * vertexNormal);
    fragNormal = normalize(mix(n, vec3(0.0, 1.0, 0.0), 0.25));
    fragColor = vec4(vertexColor.rgb, h);
    fragVar = fract(sin(dot(origin.xz, vec2(12.9898, 78.233))) * 43758.5453);
    fragRoot = origin;
    gl_Position = mvp * wp;
}
)";

static const char* FS_GRASS = R"(
in vec3 fragRoot;
uniform sampler2D texture0;
uniform float fadeStart;
uniform float fadeEnd;
void main() {
    float dist = length(fragPos - camPos);
    float fade = 1.0 - smoothstep(fadeStart, fadeEnd, dist);
    if (hash12(gl_FragCoord.xy + fragVar * 31.0) > fade) discard;
    float t = fragColor.a;   // 0 root .. 1 tip
    // painted: the ground's green at the root, bright yellow-green at the tips
    vec3 base = tuftGreen(fragRoot.xz) * (fragColor.rgb * 2.0);
    vec3 tip = base * vec3(1.25, 1.22, 0.7) + vec3(0.07, 0.08, 0.0);
    vec3 albedo = mix(base * 0.7, tip, smoothstep(0.05, 1.0, t));
    albedo = mix(albedo, albedo * vec3(1.15, 1.05, 0.75), fragVar * 0.35);   // clump to clump
    if (fragUV.x > 1.5) {   // wildflower heads
        float k = floor(fragUV.x - 2.0 + 0.5);
        albedo = k < 0.5 ? vec3(0.95, 0.80, 0.12) : k < 1.5 ? vec3(0.95, 0.95, 0.92) : k < 2.5 ? vec3(0.62, 0.35, 0.80) : vec3(0.30, 0.45, 0.95);
    }
    albedo *= albedo * 1.6;
    albedo = mix(albedo * mix(1.0, 0.7, wetness), vec3(0.75, 0.78, 0.84), snowCover * smoothstep(0.2, 0.8, t));
    vec3 n = normalize(fragNormal);
    float sh = shadowFactor(fragPos, vec3(0.0, 1.0, 0.0));
    vec3 col = shade(albedo, n, fragPos, 0.8, 0.04 + wetness * 0.3, sh, mix(0.55, 1.0, t));
    // soft wrap: light bleeds round the clump instead of a hard terminator
    float wrapL = clamp((dot(n, sunDir) + 0.5) / 1.5, 0.0, 1.0) - max(dot(n, sunDir), 0.0);
    float sunlit = sh * cloudShadow(fragPos);
    col += albedo * sunColor * wrapL * 0.6 * sunlit;
    // translucency: thin blades glow when the sun is behind them, most at the tips
    vec3 V = normalize(camPos - fragPos);
    float back = pow(max(dot(-V, sunDir), 0.0), 3.0);
    col += albedo * vec3(0.9, 1.0, 0.65) * sunColor * (0.12 + 0.45 * back) * t * t * sunlit;
    finalColor = vec4(applyFog(col, fragPos), 1.0);
}
)";

// ---------------------------------------------------------------- animals
static const char* VS_ANIMAL = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPos;
out vec2 fragUV;
out vec3 fragNormal;
out vec4 fragColor;
out float fragVar;
out vec3 fragObj;
void main() {
    fragPos = (matModel * vec4(vertexPosition, 1.0)).xyz;
    fragUV = vertexTexCoord;
    fragNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);
    fragColor = vertexColor;
    fragVar = 0.0;
    fragObj = vertexPosition;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

static const char* FS_ANIMAL = R"(
in vec3 fragObj;
uniform int species;       // 0 sheep, 1 cow, 2 deer, 3 horse
uniform vec3 coatA;        // main coat (linear)
uniform vec3 coatB;        // patches / pale parts
uniform float patchAmt;
uniform float seed;
uniform float sheen;
uniform float shadowsOn2;
void main() {
    vec3 n = normalize(fragNormal);
    if (!gl_FrontFacing) n = -n;
    vec3 p = fragObj;
    float part = fragColor.r;   // 0 coat, .2 dark hair/face, .4 hoof, .6 horn, .8 eye/nose, 1 pale
    vec3 albedo = coatA;
    float rough = 0.78, spec = 0.12 + sheen;
    if (patchAmt > 0.0) {
        float pn = fbm3(vec2(p.z * 1.6 + p.x * 0.9 + seed * 7.1, p.y * 2.1 - p.x * 0.7 + seed));
        albedo = mix(albedo, coatB, smoothstep(0.50, 0.54, pn) * patchAmt);
    }
    // hair: fine streaks running along the body
    float fur = vnoise(vec2(p.z * 34.0 + p.x * 9.0, p.y * 150.0 + p.x * 40.0));
    albedo *= 0.86 + 0.28 * fur;
    if (species == 0 && part < 0.1) {
        // wool: curly bumps, a little grubby underneath
        float w1 = vnoise(p.xz * 48.0 + p.y * 31.0), w2 = vnoise(p.zy * 51.0 + p.x * 27.0), w3 = vnoise(p.xy * 23.0 + 5.0);
        n = normalize(n + (vec3(w1, w2, w3) - 0.5) * 1.1);
        albedo *= 0.78 + 0.32 * (w1 * 0.6 + w3 * 0.4);
        albedo *= mix(0.72, 1.0, smoothstep(0.42, 0.75, p.y));
        rough = 0.95; spec = 0.03;
    }
    if (part > 0.1 && part < 0.3) { albedo = species == 0 ? vec3(0.006, 0.005, 0.005) : coatA * coatA * 2.5 + vec3(0.004); rough = 0.7; }
    else if (part > 0.3 && part < 0.5) { albedo = vec3(0.012, 0.010, 0.009); rough = 0.45; spec = 0.35; }
    else if (part > 0.5 && part < 0.7) { albedo = vec3(0.45, 0.40, 0.30) * (0.8 + 0.2 * fur); rough = 0.4; spec = 0.4; }
    else if (part > 0.7 && part < 0.9) { albedo = vec3(0.004); rough = 0.06; spec = 1.4; }
    else if (part > 0.9) albedo = mix(albedo, coatB, 0.9);
    albedo = mix(albedo, vec3(0.78, 0.81, 0.86), snowCover * smoothstep(0.55, 0.95, n.y) * 0.6);
    float sh = shadowFactor(fragPos, n);
    vec3 col = shade(albedo, n, fragPos, rough, spec, sh, 1.0);
    // soft fur rim
    vec3 V = normalize(camPos - fragPos);
    col += albedo * sunColor * pow(1.0 - max(dot(n, V), 0.0), 3.0) * 0.25 * sh;
    finalColor = vec4(applyFog(col, fragPos), 1.0);
}
)";

// ---------------------------------------------------------------- rider kit (region per vertex)
static const char* FS_KIT = R"(
uniform vec3 kitJersey;
uniform vec3 kitAccent;
uniform vec3 kitShorts;
uniform vec3 kitSkin;
uniform vec3 kitShoe;
uniform float ghost;
void main() {
    vec3 n = normalize(fragNormal);
    int reg = int(fragColor.r * 255.0 + 0.5);
    vec3 c = kitJersey;
    float rough = 0.62, spec = 0.22;
    if (reg == 1) c = kitAccent;
    else if (reg == 2) { c = kitShorts; rough = 0.42; spec = 0.5; }
    else if (reg == 3) { c = kitSkin; rough = 0.55; spec = 0.22; }
    else if (reg == 4) { c = kitShoe; rough = 0.3; spec = 0.6; }
    else if (reg == 5) { c = vec3(0.92); rough = 0.85; spec = 0.05; }
    else if (reg == 6) { c = vec3(0.07); rough = 0.6; spec = 0.25; }
    else if (reg == 7) { c = vec3(0.04); rough = 0.5; spec = 0.2; }
    if (reg <= 2 || reg == 5) c *= 0.93 + 0.07 * vnoise(vec2(fragPos.x + fragPos.z, fragPos.y) * 260.0);
    vec3 albedo = c * c;
    float sh = shadowFactor(fragPos, n);
    vec3 col = shade(albedo, n, fragPos, rough, spec, sh, 1.0);
    if (reg == 3) col += albedo * sunColor * (dot(n, sunDir) * 0.5 + 0.5) * 0.12 * sh;   // soft wrap on skin
    vec3 V = normalize(camPos - fragPos);
    if (reg == 2) col += albedo * sunColor * pow(1.0 - max(dot(n, V), 0.0), 4.0) * 0.5 * sh;  // lycra sheen
    if (ghost > 0.5) col = col * 0.45 + vec3(0.22, 0.5, 1.0) * (0.25 + 0.35 * pow(1.0 - max(dot(n, V), 0.0), 2.0));
    finalColor = vec4(applyFog(col, fragPos), 1.0);
}
)";

// ---------------------------------------------------------------- lantern light shafts (additive)
static const char* FS_VOLUME = R"(
uniform vec3 volColor;
uniform float volDensity;
void main() {
    float v = fragUV.y;   // 0 at the lamp, 1 at the ground
    vec3 n = normalize(fragNormal);
    vec3 V = normalize(camPos - fragPos);
    float edge = pow(abs(dot(n, V)), 1.6);
    float dust = 0.7 + 0.3 * vnoise(fragPos.xz * 1.7 + vec2(time * 0.35, -time * 0.25) + fragPos.y * 0.8);
    float a = smoothstep(0.0, 0.06, v) * pow(1.0 - v, 1.6) * edge * dust * volDensity;
    float d = length(fragPos - camPos);
    a *= smoothstep(1.5, 5.0, d) * exp(-d * fogDensity * 0.7);
    finalColor = vec4(volColor * a, 1.0);
}
)";

// ---------------------------------------------------------------- textured particles (default vertex shader)
static const char* FS_PARTICLE = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform float intensity;
out vec4 finalColor;
void main() {
    vec4 t = texture(texture0, fragTexCoord);
    finalColor = vec4(fragColor.rgb * intensity, t.a * fragColor.a);
}
)";

// ---------------------------------------------------------------- post processing
static const char* FS_POST_HEADER = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
out vec4 finalColor;
)";

// scene colour + linear depth (alpha) for the water pass
static const char* FS_REFR_COPY = R"(
uniform sampler2D sceneDepth;
uniform vec2 clipNF;
void main() {
    float d = texture(sceneDepth, fragTexCoord).r * 2.0 - 1.0;
    float lin = 2.0 * clipNF.x * clipNF.y / (clipNF.y + clipNF.x - d * (clipNF.y - clipNF.x));
    finalColor = vec4(texture(texture0, fragTexCoord).rgb, lin);
}
)";

static const char* FS_BRIGHT = R"(
uniform vec2 texel;
uniform float threshold;
void main() {
    vec3 c = texture(texture0, fragTexCoord + texel * vec2(-0.5, -0.5)).rgb
           + texture(texture0, fragTexCoord + texel * vec2(0.5, -0.5)).rgb
           + texture(texture0, fragTexCoord + texel * vec2(-0.5, 0.5)).rgb
           + texture(texture0, fragTexCoord + texel * vec2(0.5, 0.5)).rgb;
    c *= 0.25;
    c = min(c, vec3(60.0));
    float l = max(c.r, max(c.g, c.b));
    float knee = threshold * 0.5;
    float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contrib = max(soft, l - threshold) / max(l, 1e-4);
    finalColor = vec4(c * contrib, 1.0);
}
)";

static const char* FS_BLUR = R"(
uniform vec2 dir;   // texel-scaled direction
void main() {
    vec3 c = texture(texture0, fragTexCoord).rgb * 0.2270270270;
    c += texture(texture0, fragTexCoord + dir * 1.3846153846).rgb * 0.3162162162;
    c += texture(texture0, fragTexCoord - dir * 1.3846153846).rgb * 0.3162162162;
    c += texture(texture0, fragTexCoord + dir * 3.2307692308).rgb * 0.0702702703;
    c += texture(texture0, fragTexCoord - dir * 3.2307692308).rgb * 0.0702702703;
    finalColor = vec4(c, 1.0);
}
)";

static const char* FS_COMPOSITE = R"(
uniform sampler2D bloomA;
uniform sampler2D bloomB;
uniform vec2 sunUV;
uniform float sunVisible;
uniform float exposure;
uniform float bloomStrength;
uniform float shaftStrength;
uniform float saturation;
uniform float rainAmt;
uniform float snowAmt;
uniform float lensDrops;
uniform float ptime;
uniform float windX;
uniform float aspect;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
    vec2 uv = fragTexCoord;
    vec3 c = texture(texture0, uv).rgb;
    // raindrops sitting on the lens, refracting the scene upside down
    if (lensDrops * rainAmt > 0.01) {
        vec2 g = vec2(uv.x * aspect, uv.y) * 6.0;
        vec2 id = floor(g);
        vec2 f = fract(g) - 0.5;
        float h = hash(id);
        if (h < rainAmt * 0.5) {
            float life = fract(ptime * (0.05 + 0.05 * h) + h * 13.0);
            vec2 ctr = (vec2(hash(id + 3.1), hash(id + 7.7)) - 0.5) * 0.55 - vec2(0.0, life * 0.12);
            float r = mix(0.07, 0.19, hash(id + 1.7));
            vec2 dv = (f - ctr) / vec2(r, r * 1.15);
            float d = length(dv);
            if (d < 1.0) {
                float z = sqrt(1.0 - d * d);
                vec3 refr = texture(texture0, uv - dv * 0.045 * z).rgb;
                float a = smoothstep(0.0, 0.1, life) * (1.0 - smoothstep(0.75, 1.0, life)) * smoothstep(1.0, 0.85, d);
                c = mix(c, refr * 0.95 + vec3(0.02) * pow(z, 8.0), a);
            }
        }
    }
    vec3 b = texture(bloomA, uv).rgb * 0.55 + texture(bloomB, uv).rgb * 0.45;
    c += b * bloomStrength;
    if (sunVisible > 0.001) {
        vec2 delta = (uv - sunUV) * (0.9 / 48.0);
        vec2 tc = uv;
        float illum = 1.0;
        vec3 acc = vec3(0.0);
        for (int i = 0; i < 48; i++) {
            tc -= delta;
            acc += texture(bloomA, tc).rgb * illum;
            illum *= 0.955;
        }
        c += acc / 48.0 * shaftStrength * sunVisible;
    }
    float lum = dot(texture(bloomB, uv).rgb + c * 0.3, vec3(0.3, 0.5, 0.2));
    // distant rain falling in sheets
    if (rainAmt > 0.01) {
        float rn = 0.0;
        for (int k = 0; k < 2; k++) {
            float cols = 150.0 + float(k) * 110.0;
            float x = (uv.x + uv.y * windX * 0.12) * cols;
            float id = floor(x);
            float h = hash(vec2(id, float(k) * 7.0));
            float fx = abs(fract(x) - 0.5);
            float y = fract(uv.y * (1.2 + h) + ptime * (1.7 + h * 1.3 + float(k) * 0.6) + h * 10.0);
            float s = smoothstep(0.0, 0.03, y) * (1.0 - smoothstep(0.03, 0.22, y)) * step(0.62, h) * (1.0 - smoothstep(0.1, 0.3, fx));
            rn += s * (0.6 - float(k) * 0.25);
        }
        c += rn * rainAmt * (0.06 + lum * 0.35);
    }
    // distant snow drifting down in three depth layers
    if (snowAmt > 0.01) {
        float sn = 0.0;
        for (int k = 0; k < 3; k++) {
            float sc = 14.0 + float(k) * 11.0;
            vec2 q = vec2(uv.x * aspect, uv.y) * sc + vec2(ptime * windX * (0.6 + float(k) * 0.3), ptime * (0.9 + float(k) * 0.35));
            q.x += sin(q.y * 0.7 + float(k)) * 0.35;
            vec2 id = floor(q), f = fract(q) - 0.5;
            float h = hash(id + float(k) * 3.3);
            vec2 o = (vec2(hash(id + 1.3), hash(id + 5.9)) - 0.5) * 0.7;
            float d = length(f - o);
            sn += smoothstep(0.07 - float(k) * 0.015, 0.0, d) * step(0.45, h) * (1.0 - float(k) * 0.25);
        }
        c += sn * snowAmt * (0.12 + lum * 0.6);
    }
    c *= exposure;
    c = aces(c);
    c = pow(c, vec3(1.0 / 2.2));
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, saturation);
    c = (c - 0.5) * 1.04 + 0.5;
    vec2 vv = uv - 0.5;
    c *= mix(1.0, smoothstep(0.85, 0.25, length(vv * vec2(1.1, 1.0))), 0.35);
    c += (hash(uv * 1000.0) - 0.5) / 255.0;  // dither against banding
    finalColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

static const char* FS_HASH = R"(
float hash(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
)";

static const char* FS_FXAA = R"(
uniform vec2 texel;
uniform float sharpen;
void main() {
    vec2 uv = fragTexCoord;
    const vec3 L = vec3(0.299, 0.587, 0.114);
    vec3 rgbNW = texture(texture0, uv + vec2(-1.0, -1.0) * texel).rgb;
    vec3 rgbNE = texture(texture0, uv + vec2(1.0, -1.0) * texel).rgb;
    vec3 rgbSW = texture(texture0, uv + vec2(-1.0, 1.0) * texel).rgb;
    vec3 rgbSE = texture(texture0, uv + vec2(1.0, 1.0) * texel).rgb;
    vec3 rgbM = texture(texture0, uv).rgb;
    float lNW = dot(rgbNW, L), lNE = dot(rgbNE, L), lSW = dot(rgbSW, L), lSE = dot(rgbSE, L), lM = dot(rgbM, L);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * texel;
    vec3 a = 0.5 * (texture(texture0, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(texture0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(texture0, uv + dir * -0.5).rgb + texture(texture0, uv + dir * 0.5).rgb);
    float lB = dot(b, L);
    vec3 c = (lB < lMin || lB > lMax) ? a : b;
    if (sharpen > 0.0) {
        // contrast-adaptive sharpen (after AMD's CAS): push away from the local average, less
        // where the neighbourhood already has strong contrast, never past its min / max
        vec3 avg = (rgbNW + rgbNE + rgbSW + rgbSE) * 0.25;
        vec3 mn = min(rgbM, min(min(rgbNW, rgbNE), min(rgbSW, rgbSE)));
        vec3 mx = max(rgbM, max(max(rgbNW, rgbNE), max(rgbSW, rgbSE)));
        vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, 1e-3), 0.0, 1.0);
        c = clamp(c + (c - avg) * sharpen * sqrt(amp), mn, mx);
    }
    finalColor = vec4(c, 1.0);
}
)";

}  // namespace shaders
