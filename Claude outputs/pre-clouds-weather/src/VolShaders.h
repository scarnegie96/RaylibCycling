#pragma once
// Volumetric fog, rain shafts and clouds (Volumetrics.cpp).
//
// After Schneider / Hillaire as used by harkawataryan/Volumetric-Cloud-Renderer: a tiling
// Perlin-Worley base shape eroded by a Worley detail field, coverage + cloud type from a
// weather map, a curved shell around a (small) planet, six light samples towards the SUN per
// step, four multiple-scattering octaves, a two-lobe Henyey-Greenstein phase, a powder term,
// analytic per-step in-scattering and a horizon haze fade. GL 3.3 has no 3D textures through
// raylib, so the 3D noise lives in 2D atlases of slices (with a one-texel wrap gutter).
//
// The same march also integrates near-field fog / mist / rain spray lit by the sun or moon
// (shadowed by the shadow cascades => god rays through the trees) and by the lamps and the
// headlight (shaped spots => visible light cones), and far rain shafts under the clouds.
// It runs at reduced resolution with a per-frame jitter, then a temporal resolve
// (reprojection + neighbourhood clamp) and a depth-aware upsample onto the scene.

namespace volshaders {

static const char* FS_VOL_MARCH = R"(#version 330
out vec4 finalColor;
uniform vec2 res;
uniform sampler2D depthTex;
uniform vec2 clipNF;
uniform vec3 camPos;
uniform vec3 camFwd;
uniform vec3 camRight;
uniform vec3 camUp;
uniform float tanHalf;
uniform float aspect;
uniform float frameIdx;
uniform float time;

uniform sampler2D shapeTex;    // 128^3: r = Perlin-Worley, gba = Worley at 4 / 8 / 16 cells
uniform sampler2D detailTex;   // 32^3:  rgb = Worley at 4 / 8 / 16 cells
uniform sampler2D weatherTex;  // r = coverage, g = cloud type, b = precipitation cells
uniform vec4 shapeDim;         // N, columns, atlas w, atlas h
uniform vec4 detailDim;

// lighting
uniform vec3 sunDir;           // the real sun (clouds are lit by it alone)
uniform vec3 sunCloudCol;      // sunlight above the clouds
uniform vec3 lightDirF;        // the directional light under the clouds (sun by day, moon by night)
uniform vec3 lightColF;
uniform vec3 skyZenith;
uniform vec3 skyHorizon;
uniform vec3 groundColor;
uniform float flash;
uniform float fogGrey;

// clouds
uniform float cloudOn;
uniform float coverage;
uniform float cloudType;       // 0 stratus .. 0.5 cumulus .. 1 cumulonimbus
uniform float cloudBase;       // world y
uniform float cloudTop;
uniform float cloudSigma;      // extinction per metre at density 1
uniform float cloudDark;
uniform float weatherScale;    // metres per weather-map tile
uniform vec2 windOffset;       // metres the clouds have drifted
uniform float hazeDensity;
uniform float cloudSteps;
uniform float planetR;
uniform float volDebug;

// fog / rain
uniform float fogDens;         // extinction per metre at the reference height
uniform float fogBase;         // world y of the valley floor around the rider
uniform float fogScaleH;       // e-folding height (m)
uniform float fogRange;
uniform float fogSteps;
uniform float rainAmt;
uniform float snowAmt;
uniform float precip;
uniform vec3 windVel;

// sun shadows for the fog
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform mat4 lightVP0;
uniform mat4 lightVP1;
uniform vec3 cascadeCenter0;
uniform vec3 cascadeCenter1;
uniform vec2 cascadeRange;

// lamps + headlight
uniform int numLights;
uniform vec3 lightPos[16];
uniform vec3 lightCol[16];
uniform vec3 lightDir[16];
uniform vec2 lightCone[16];
uniform vec4 lightShape[16];   // xyz = beam's long axis (along the road), w = elongation
uniform float lightVol[16];    // how strongly each light shows in the air (headlight: faint)

const float PI = 3.14159265;

float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
float remap(float x, float a, float b, float c, float d) { return c + (x - a) / max(b - a, 1e-5) * (d - c); }

vec4 tex3D(sampler2D t, vec3 p, vec4 dim) {
    float N = dim.x, cols = dim.y, T = N + 2.0;
    p = fract(p);
    float z = p.z * N - 0.5;
    float z0 = floor(z), f = z - z0;
    float s0 = mod(z0 + N, N), s1 = mod(z0 + 1.0, N);
    vec2 inT = 1.0 + p.xy * N;
    vec2 a0 = (vec2(mod(s0, cols), floor(s0 / cols)) * T + inT) / dim.zw;
    vec2 a1 = (vec2(mod(s1, cols), floor(s1 / cols)) * T + inT) / dim.zw;
    return mix(textureLod(t, a0, 0.0), textureLod(t, a1, 0.0), f);
}

// Henyey-Greenstein normalised so isotropic = 1
float hg(float c, float g) { float g2 = g * g; return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5); }
float phase2(float c, float k) { return mix(hg(c, 0.78 * k), hg(c, -0.25 * k), 0.3); }

// ------------------------------------------------------------ clouds
float covMask(float w) { return smoothstep(1.0 - coverage - 0.04, 1.0 - coverage + 0.32, w); }

float heightGrad(float h, float type) {
    float st = smoothstep(0.0, 0.07, h) * (1.0 - smoothstep(0.16, 0.32, h));
    float cu = smoothstep(0.0, 0.06, h) * (1.0 - smoothstep(0.30, 0.92, h));   // flat bases, rounded tops
    float cb = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.62, 1.0, h));
    return type < 0.5 ? mix(st, cu, type * 2.0) : mix(cu, cb, type * 2.0 - 1.0);
}

float altitude(vec3 p) {
    vec3 q = vec3(p.x - camPos.x, p.y + planetR, p.z - camPos.z);
    return length(q) - planetR;
}

float cloudDensity(vec3 p, bool detail) {
    float h = (altitude(p) - cloudBase) / (cloudTop - cloudBase);
    if (h < 0.0 || h > 1.0) return 0.0;
    vec3 w = textureLod(weatherTex, (p.xz + windOffset) / weatherScale, 0.0).rgb;
    float cov = covMask(w.r);
    if (cov < 0.002) return 0.0;
    float type = clamp(cloudType + (w.g - 0.5) * 0.45, 0.0, 1.0);
    // wind shear: the tops run ahead of the bases
    vec3 sp = p + vec3(windOffset.x, 0.0, windOffset.y) * (1.0 + h * 0.35);
    vec4 s = tex3D(shapeTex, sp / 5200.0, shapeDim);
    float wf = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
    float sr = smoothstep(0.48, 0.86, s.r);   // the Perlin-Worley field is narrow (p10..p90 = 0.55..0.8): stretch it
    float base = remap(sr, wf - 1.0, 1.0, 0.0, 1.0);
    base *= heightGrad(h / mix(0.4, 1.0, cov), type);   // thinner coverage -> shorter cloud: edges dome over
    base = clamp(remap(base, 1.0 - cov, 1.0, 0.0, 1.0), 0.0, 1.0) * cov;
    if (base <= 0.0 || !detail) return base;
    vec4 d = tex3D(detailTex, sp / 820.0 + vec3(0.0, -time * 0.0035, 0.0), detailDim);
    float df = smoothstep(0.25, 0.7, d.r * 0.625 + d.g * 0.25 + d.b * 0.125);
    float m = mix(df, 1.0 - df, clamp(h * 6.0, 0.0, 1.0));   // wispy bases, billowy tops
    return clamp(remap(base, m * 0.55 * (1.0 - cov * 0.5), 1.0, 0.0, 1.0), 0.0, 1.0) * 1.6;
}

// optical depth towards the sun: six samples, growing steps
float sunOpticalDepth(vec3 p) {
    float thick = cloudTop - cloudBase;
    float od = 0.0, prev = 0.0;
    const float k[6] = float[](0.04, 0.10, 0.19, 0.33, 0.55, 0.95);
    for (int i = 0; i < 6; i++) {
        float d = k[i] * thick;
        od += cloudDensity(p + sunDir * d, i < 3) * (d - prev);
        prev = d;
    }
    return od;
}

// ray / sphere (centre below the camera), returns the far root; c computed without cancellation
float shellHit(float camAlt, float shellAlt, float dy, out bool hit) {
    float R = planetR;
    float b = (R + camAlt) * dy;
    float c = (camAlt - shellAlt) * (2.0 * R + camAlt + shellAlt);
    float disc = b * b - c;
    hit = disc >= 0.0;
    float sq = sqrt(max(disc, 0.0));
    return c < 0.0 ? -b + sq : -b - sq;   // inside: the exit; outside: the near entry
}

vec4 marchClouds(vec3 ro, vec3 rd, float tScene, float jitter, out float avgT) {
    avgT = 0.0;
    float camAlt = ro.y;
    if (camAlt > cloudTop) return vec4(0.0, 0.0, 0.0, 1.0);
    bool h0, h1;
    float t0, t1;
    if (camAlt < cloudBase) {
        if (rd.y < -0.02) return vec4(0.0, 0.0, 0.0, 1.0);
        t0 = shellHit(camAlt, cloudBase, rd.y, h0);
        t1 = shellHit(camAlt, cloudTop, rd.y, h1);
    } else {
        t0 = 0.0;
        t1 = shellHit(camAlt, cloudTop, rd.y, h1);
        if (rd.y < 0.0) { bool hb; float tb = shellHit(camAlt, cloudBase, rd.y, hb); if (hb && tb > 0.0) t1 = min(t1, tb); }
    }
    t1 = min(t1, 60000.0);
    t0 = max(t0, 0.0);
    if (t0 >= tScene || t1 <= t0) return vec4(0.0, 0.0, 0.0, 1.0);
    t1 = min(t1, tScene);
    float len = t1 - t0;
    int n = int(clamp(len / 70.0, 16.0, cloudSteps));
    float ds = len / float(n);
    float t = t0 + ds * jitter;
    float cosT = dot(rd, sunDir);
    vec3 ambTop = skyZenith * 0.8 + skyHorizon * 0.25;
    vec3 ambBot = skyHorizon * 0.28 + groundColor * 0.45;
    float ambK = 1.0 - 0.55 * cloudDark;
    vec3 scat = vec3(0.0);
    float T = 1.0, wsum = 0.0, tsum = 0.0;
    int empty = 0;
    for (int i = 0; i < 160; i++) {
        if (i >= n || T < 0.01) break;
        vec3 p = ro + rd * t;
        float dc = cloudDensity(p, false);
        if (dc > 0.0) {
            float dens = cloudDensity(p, true);
            if (dens > 0.001) {
                float sig = dens * cloudSigma;
                float od = sunOpticalDepth(p);
                // multiple scattering octaves (Wrenninge / Hillaire)
                float a = 1.0, b = 1.0, c = 1.0, lum = 0.0;
                for (int o = 0; o < 4; o++) {
                    lum += b * phase2(cosT, c) * exp(-od * cloudSigma * 0.3 * a);   // light extinction thinned: forward-peaked scattering keeps light going
                    a *= 0.45; b *= 0.6; c *= 0.5;
                }
                // powder: dark, thin edges when looking away from the sun; silver lining towards it
                float powder = 1.0 - exp(-dens * 2.2);
                powder = mix(powder * 1.6, 1.0, smoothstep(-0.1, 0.85, cosT));
                float h = clamp((altitude(p) - cloudBase) / (cloudTop - cloudBase), 0.0, 1.0);
                vec3 amb = mix(ambBot, ambTop, h) * ambK + vec3(0.75, 0.8, 1.0) * flash * 2.5;
                vec3 S = (sunCloudCol * lum * powder * 1.15 + amb) * sig;
                float tr = exp(-sig * ds);
                scat += T * (S - S * tr) / sig;
                float absorbed = T * (1.0 - tr);
                tsum += t * absorbed; wsum += absorbed;
                T *= tr;
            }
            empty = 0;
        } else empty++;
        // empty air: stride faster until we bump into something
        t += ds * (empty > 3 ? 2.0 : 1.0);
        if (t > t1) break;
    }
    avgT = wsum > 1e-4 ? tsum / wsum : t1;
    // aerial perspective: far clouds dissolve into the horizon haze
    float fade = exp(-avgT * hazeDensity * 0.22);
    return vec4(scat * fade, 1.0 - fade * (1.0 - T));
}

// ------------------------------------------------------------ fog, lights, rain
float sunVis(vec3 p) {
    vec3 d0 = abs(p - cascadeCenter0);
    vec4 lp;
    if (max(d0.x, d0.z) < cascadeRange.x * 0.9) {
        lp = lightVP0 * vec4(p, 1.0);
        vec3 q = lp.xyz / lp.w * 0.5 + 0.5;
        if (q.x > 0.0 && q.x < 1.0 && q.y > 0.0 && q.y < 1.0 && q.z < 1.0) return q.z - 0.0008 > textureLod(shadowMap0, q.xy, 0.0).r ? 0.0 : 1.0;
    }
    vec3 d1 = abs(p - cascadeCenter1);
    if (max(d1.x, d1.z) < cascadeRange.y * 0.93) {
        lp = lightVP1 * vec4(p, 1.0);
        vec3 q = lp.xyz / lp.w * 0.5 + 0.5;
        if (q.x > 0.0 && q.x < 1.0 && q.y > 0.0 && q.y < 1.0 && q.z < 1.0) return q.z - 0.0012 > textureLod(shadowMap1, q.xy, 0.0).r ? 0.0 : 1.0;
    }
    return 1.0;
}

float cloudShadowAt(vec3 p) {
    if (cloudOn < 0.5 || lightDirF.y < 0.03) return 1.0;
    float hMid = mix(cloudBase, cloudTop, 0.25);
    vec2 xz = p.xz + lightDirF.xz / lightDirF.y * (hMid - p.y);
    float w = textureLod(weatherTex, (xz + windOffset) / weatherScale, 2.0).r;
    return 1.0 - 0.82 * covMask(w);
}

float fogDensity(vec3 p) {
    float hgt = p.y - fogBase;
    float hf = exp(-max(hgt, -15.0) / fogScaleH);
    vec3 q = p - windVel * time;
    float banks = tex3D(shapeTex, q / 260.0, shapeDim).r;
    float wisps = tex3D(detailTex, q / 38.0 + vec3(0.0, time * 0.004, 0.0), detailDim).r;
    float het = clamp(banks * 1.9 - 0.55 + (wisps - 0.5) * 0.7, 0.04, 2.2);
    float d = fogDens * hf * het;
    // spray and mist hanging in falling rain, snow haze
    d += (rainAmt * 0.0007 + snowAmt * 0.0022) * exp(-max(hgt, 0.0) / 140.0) * (0.75 + 0.5 * wisps);
    return d;
}

vec3 lampScatter(vec3 p, vec3 rd) {
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 16; i++) {
        if (i >= numLights) break;
        vec3 l = lightPos[i] - p;
        float d2 = dot(l, l);
        if (d2 > 3600.0) continue;
        float d = sqrt(d2);
        vec3 ld = l / max(d, 1e-3);
        float win = clamp(1.0 - pow(d / 60.0, 4.0), 0.0, 1.0);
        float att = win * win / (1.0 + d2 * 0.18);
        float spot = 1.0;
        if (lightCone[i].y > -0.5 && lightShape[i].w <= 1.001) {
            spot = smoothstep(lightCone[i].y, lightCone[i].x, dot(-ld, lightDir[i]));
            spot *= spot;
        } else if (lightCone[i].y > -0.5) {
            vec3 r = -ld;
            vec3 D = lightDir[i];
            vec3 A = lightShape[i].xyz;
            vec3 B = normalize(cross(D, A));
            float al = dot(r, A), ac = dot(r, B), ad = dot(r, D);
            vec3 rr = normalize(vec3(al / max(lightShape[i].w, 1.0), ac, ad));
            spot = smoothstep(lightCone[i].y, lightCone[i].x, rr.z);
            spot *= spot * (1.0 + 0.7 * exp(-pow((abs(al) - 0.55) / 0.2, 2.0)));
            // nothing above the lamp head's rim
            spot *= smoothstep(-0.05, 0.12, ad);
        }
        // mildly forward-scattering: the beam glows more looking into it
        float ph = hg(dot(rd, -ld), 0.35);
        acc += lightCol[i] * att * spot * ph * lightVol[i];
    }
    return acc;
}

vec4 marchFog(vec3 ro, vec3 rd, float tScene, float jitter) {
    float tEnd = min(tScene, fogRange);
    int n = int(fogSteps);
    vec3 scat = vec3(0.0);
    float T = 1.0;
    float cosL = dot(rd, lightDirF);
    float ph = mix(hg(cosL, 0.62), 1.0, 0.35);
    vec3 amb = (skyHorizon * 0.65 + skyZenith * 0.35);
    amb = mix(amb, vec3(dot(amb, vec3(0.333))), fogGrey * 0.7) * (1.1 - 0.55 * cloudDark) + vec3(0.75, 0.8, 1.0) * flash * 2.0;
    for (int i = 0; i < 64; i++) {
        if (i >= n || T < 0.02) break;
        float a = float(i) / float(n), b = float(i + 1) / float(n);
        float ta = tEnd * a * a, tb = tEnd * b * b;
        float t = mix(ta, tb, jitter), ds = tb - ta;
        vec3 p = ro + rd * t;
        float sig = fogDensity(p);
        if (sig < 1e-6) continue;
        vec3 Li = lightColF * ph * sunVis(p) * cloudShadowAt(p) + amb;
        Li += lampScatter(p, rd);
        vec3 S = Li * sig;
        float tr = exp(-sig * ds);
        scat += T * (S - S * tr) / sig;
        T *= tr;
    }
    return vec4(scat, T);
}

// rain falling out of the clouds in grey curtains, beyond the fog volume
vec4 marchShafts(vec3 ro, vec3 rd, float tScene, float jitter) {
    if (precip < 0.01 || cloudOn < 0.5) return vec4(0.0, 0.0, 0.0, 1.0);
    float t0 = fogRange, t1 = min(tScene, 14000.0);
    if (rd.y > 0.0) t1 = min(t1, (cloudBase - ro.y) / max(rd.y, 1e-3));
    if (t1 <= t0) return vec4(0.0, 0.0, 0.0, 1.0);
    const int n = 14;
    float ds = (t1 - t0) / float(n);
    vec3 scat = vec3(0.0);
    float T = 1.0;
    vec3 amb = (skyHorizon * 0.6 + skyZenith * 0.25) * (1.0 - 0.4 * cloudDark);
    for (int i = 0; i < n; i++) {
        float t = t0 + (float(i) + jitter) * ds;
        vec3 p = ro + rd * t;
        if (p.y > cloudBase) break;
        // rain slants with the wind as it falls
        vec2 xz = p.xz + windVel.xz * (cloudBase - p.y) * 0.12;
        vec3 w = textureLod(weatherTex, (xz + windOffset) / weatherScale, 1.0).rgb;
        float cell = covMask(w.r) * smoothstep(0.35, 0.75, w.b);
        float streak = tex3D(detailTex, vec3(xz / 900.0, 0.37), detailDim).g;
        float sig = precip * 0.00055 * cell * (0.4 + 1.2 * streak) * smoothstep(cloudBase + 50.0, cloudBase - 300.0, p.y);
        if (sig < 1e-7) continue;
        vec3 S = (amb + lightColF * 0.15) * sig;
        float tr = exp(-sig * ds);
        scat += T * (S - S * tr) / sig;
        T *= tr;
    }
    return vec4(scat, T);
}

void main() {
    vec2 uv = gl_FragCoord.xy / res;
    float z = textureLod(depthTex, uv, 0.0).r;
    float n = clipNF.x, f = clipNF.y;
    float lin = 2.0 * n * f / (f + n - (z * 2.0 - 1.0) * (f - n));
    vec3 rd = normalize(camFwd + (uv.x * 2.0 - 1.0) * tanHalf * aspect * camRight + (uv.y * 2.0 - 1.0) * tanHalf * camUp);
    float tScene = z >= 0.99999 ? 1e7 : lin / max(dot(rd, camFwd), 1e-3);
    float j = ign(gl_FragCoord.xy + frameIdx * vec2(5.588238, 3.17));

    vec4 fog = marchFog(camPos, rd, tScene, j);
    vec4 sh = marchShafts(camPos, rd, tScene, fract(j + 0.37));
    vec4 cl = vec4(0.0, 0.0, 0.0, 1.0);
    float avgT;
    if (cloudOn > 0.5) cl = marchClouds(camPos, rd, tScene, fract(j + 0.61), avgT);
    // front to back: fog, then rain shafts, then clouds
    vec3 S = fog.rgb + fog.a * (sh.rgb + sh.a * cl.rgb);
    float T = fog.a * sh.a * cl.a;
    finalColor = vec4(S, T);
    if (volDebug > 0.5) {
        // r: cloud opacity, g: coverage straight above, b: shape noise straight above
        vec3 above = vec3(camPos.x, mix(cloudBase, cloudTop, 0.3), camPos.z) + rd * 3000.0;
        float wr = textureLod(weatherTex, (above.xz + windOffset) / weatherScale, 0.0).r;
        float sh = tex3D(shapeTex, above / 5200.0, shapeDim).r;
        finalColor = vec4(1.0 - cl.a, covMask(wr), sh, 0.0);
    }
}
)";

// temporal resolve at the march resolution
static const char* FS_VOL_RESOLVE = R"(#version 330
out vec4 finalColor;
uniform sampler2D curTex;
uniform sampler2D histTex;
uniform sampler2D depthTex;
uniform vec2 res;
uniform vec2 clipNF;
uniform vec3 camPos;
uniform vec3 camFwd;
uniform vec3 camRight;
uniform vec3 camUp;
uniform float tanHalf;
uniform float aspect;
uniform mat4 prevVP;
uniform float histValid;
uniform float histWeight;
void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    ivec2 mx = ivec2(res) - 1;
    vec4 cur = texelFetch(curTex, ip, 0);
    vec4 lo = cur, hi = cur;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) continue;
            vec4 c = texelFetch(curTex, clamp(ip + ivec2(x, y), ivec2(0), mx), 0);
            lo = min(lo, c); hi = max(hi, c);
        }
    vec2 uv = gl_FragCoord.xy / res;
    float z = textureLod(depthTex, uv, 0.0).r;
    float n = clipNF.x, f = clipNF.y;
    float lin = 2.0 * n * f / (f + n - (z * 2.0 - 1.0) * (f - n));
    vec3 rd = normalize(camFwd + (uv.x * 2.0 - 1.0) * tanHalf * aspect * camRight + (uv.y * 2.0 - 1.0) * tanHalf * camUp);
    float dist = z >= 0.99999 ? 9000.0 : lin / max(dot(rd, camFwd), 1e-3);
    vec4 pc = prevVP * vec4(camPos + rd * dist, 1.0);
    vec4 outc = cur;
    if (histValid > 0.5 && pc.w > 0.0) {
        vec2 puv = pc.xy / pc.w * 0.5 + 0.5;
        if (puv.x > 0.0 && puv.x < 1.0 && puv.y > 0.0 && puv.y < 1.0) {
            vec4 h = clamp(texture(histTex, puv), lo, hi);
            outc = mix(cur, h, histWeight);
        }
    }
    finalColor = outc;
}
)";

// depth-aware upsample onto the full-resolution scene: colour * transmittance + in-scatter
static const char* FS_VOL_APPLY = R"(#version 330
out vec4 finalColor;
uniform sampler2D sceneTex;
uniform sampler2D volTex;
uniform sampler2D depthTex;
uniform vec2 fullRes;
uniform vec2 lowRes;
uniform vec2 clipNF;
float linZ(vec2 uv) {
    float z = textureLod(depthTex, uv, 0.0).r;
    float n = clipNF.x, f = clipNF.y;
    return 2.0 * n * f / (f + n - (z * 2.0 - 1.0) * (f - n));
}
void main() {
    vec2 uv = gl_FragCoord.xy / fullRes;
    vec3 c = textureLod(sceneTex, uv, 0.0).rgb;
    float dC = linZ(uv);
    vec2 lp = uv * lowRes - 0.5;
    vec2 b = floor(lp), fr = lp - b;
    ivec2 mx = ivec2(lowRes) - 1;
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int j = 0; j <= 1; j++)
        for (int i = 0; i <= 1; i++) {
            ivec2 ip = clamp(ivec2(b) + ivec2(i, j), ivec2(0), mx);
            vec4 v = texelFetch(volTex, ip, 0);
            float d = linZ((vec2(ip) + 0.5) / lowRes);
            float bil = (i == 0 ? 1.0 - fr.x : fr.x) * (j == 0 ? 1.0 - fr.y : fr.y);
            float w = bil * exp(-abs(d - dC) / (0.04 * dC + 0.4)) + 1e-5;
            acc += v * w; wsum += w;
        }
    vec4 v = acc / wsum;
    finalColor = vec4(c * v.a + v.rgb, 1.0);
}
)";

}  // namespace volshaders
