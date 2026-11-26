// Tree impostors: every tree type baked at start-up into an atlas of pictures from 8
// directions around it (albedo + normal/ao), drawn far away as one camera-facing quad per
// tree that blends the two nearest views and is lit like the real thing.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "Renderer.h"
#include "RenderUtil.h"
#pragma GCC diagnostic ignored "-Wunused-variable"   // (the shader header defines every source)
#include "Shaders.h"

using namespace rutil;

namespace {
RenderTexture2D atlasRT(int w, int h) {
    RenderTexture2D t{};
    t.id = rlLoadFramebuffer();
    rlEnableFramebuffer(t.id);
    t.texture.id = rlLoadTexture(nullptr, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    t.texture.width = w; t.texture.height = h; t.texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8; t.texture.mipmaps = 1;
    rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    t.depth.id = rlLoadTextureDepth(w, h, true);
    t.depth.width = w; t.depth.height = h;
    rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
    if (!rlFramebufferComplete(t.id)) std::fprintf(stderr, "[impostor] framebuffer incomplete\n");
    rlDisableFramebuffer();
    return t;
}
}  // namespace

void Renderer::bakeImpostors() {
    const int T = quality_ >= 2 ? 256 : quality_ == 1 ? 192 : 128;   // tile size (px)
    const int V = IMP_VIEWS, N = trees::TYPES;
    std::string head = shaders::FS_HEADER, common = shaders::FS_COMMON;
    impSh_ = LoadShaderFromMemory(shaders::VS_IMPOSTOR, (head + common + shaders::FS_IMPOSTOR).c_str());
    if (impSh_.id == rlGetShaderIdDefault()) std::fprintf(stderr, "[impostor] shader failed to compile\n");
    // raylib 5.5 reads the instance matrices through this location (as for the other instanced shaders)
    impSh_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(impSh_, "instanceTransform");
    impMat_ = LoadMaterialDefault();
    impMat_.shader = impSh_;
    // the quad
    {
        MeshBuilder b;
        b.add({-0.5f, 0, 0}, {0, 0, 1}, {0, 0});
        b.add({0.5f, 0, 0}, {0, 0, 1}, {1, 0});
        b.add({0.5f, 1, 0}, {0, 0, 1}, {1, 1});
        b.add({-0.5f, 1, 0}, {0, 0, 1}, {0, 1});
        b.tri(0, 1, 2);
        b.tri(0, 2, 3);
        impQuad_ = b.build();
    }
    RenderTexture2D alb = atlasRT(T * V, T * N), nrm = atlasRT(T * V, T * N);
    Matrix ident = MatrixIdentity();
    Shader fs = foliageSh_;
    for (int pass = 1; pass <= 2; ++pass) {
        RenderTexture2D& rt = pass == 1 ? alb : nrm;
        BeginTextureMode(rt);
        ClearBackground(BLANK);
        for (int t = 0; t < N; ++t) {
            BoundingBox bb = GetMeshBoundingBox(trees_[t].hi);
            float halfW = std::max({std::fabs(bb.min.x), std::fabs(bb.max.x), std::fabs(bb.min.z), std::fabs(bb.max.z)});
            float S = std::max(2.f * halfW, bb.max.y - bb.min.y) * 1.06f;
            float cy = 0.5f * (bb.max.y + bb.min.y);
            imp_[t] = {S, cy};
            for (int k = 0; k < V; ++k) {
                float a = 6.2831853f * k / V;
                Vector3 back{std::sin(a), 0, std::cos(a)};   // towards the camera
                Vector3 right{back.z, 0, -back.x};
                Vector3 target{0, cy, 0};
                Vector3 eye = Vector3Add(target, Vector3Scale(back, 100.f));
                rlDrawRenderBatchActive();
                rlViewport(k * T, t * T, T, T);
                rlMatrixMode(RL_PROJECTION);
                rlPushMatrix();
                rlLoadIdentity();
                rlOrtho(-S * 0.5, S * 0.5, -S * 0.5, S * 0.5, 1.0, 200.0);
                rlMatrixMode(RL_MODELVIEW);
                rlLoadIdentity();
                rlMultMatrixf(MatrixToFloat(MatrixLookAt(eye, target, {0, 1, 0})));
                rlEnableDepthTest();
                rlDisableBackfaceCulling();
                setI(fs, "bakeMode", pass);
                setV3(fs, "bakeRight", right);
                setV3(fs, "bakeBack", back);
                setI(fs, "kind", 0);
                setF(fs, "time", 0.f);
                setF(fs, "windAmount", 0.f);
                setF(fs, "treeHeight", trees_[t].height);
                DrawMeshInstanced(trees_[t].hi, foliageMat_, &ident, 1);
                rlDrawRenderBatchActive();
                rlMatrixMode(RL_PROJECTION);
                rlPopMatrix();
                rlMatrixMode(RL_MODELVIEW);
                rlLoadIdentity();
                rlEnableBackfaceCulling();
            }
        }
        EndTextureMode();
    }
    setI(fs, "bakeMode", 0);
    rlDisableDepthTest();
    // keep the colour textures, drop the framebuffers + depth
    for (RenderTexture2D* rt : {&alb, &nrm}) {
        Texture2D tex = rt->texture;
        rlUnloadFramebuffer(rt->id);   // (also frees the attached depth renderbuffer)
        GenTextureMipmaps(&tex);
        SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
        SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
        (rt == &alb ? impAlb_ : impNrm_) = tex;
    }
    if (std::getenv("CORERAY_IMPDUMP")) {
        Image a = LoadImageFromTexture(impAlb_); ExportImage(a, "imp_albedo.png"); UnloadImage(a);
        Image n = LoadImageFromTexture(impNrm_); ExportImage(n, "imp_normal.png"); UnloadImage(n);
    }
    impMat_.maps[MATERIAL_MAP_ALBEDO].texture = impAlb_;
    impMat_.maps[MATERIAL_MAP_METALNESS].texture = impNrm_;
    setV2(impSh_, "impGrid", {float(V), float(N)});
    setF(impSh_, "impViews", float(V));
}

void Renderer::unloadImpostors() {
    if (impAlb_.id) { UnloadTexture(impAlb_); impAlb_ = Texture2D{}; }
    if (impNrm_.id) { UnloadTexture(impNrm_); impNrm_ = Texture2D{}; }
    if (impQuad_.vaoId) { UnloadMesh(impQuad_); impQuad_ = Mesh{}; }
    if (impSh_.id) { UnloadShader(impSh_); impSh_ = Shader{}; }
}

void Renderer::drawImpostors(int set, const std::vector<Matrix>& inst) {
    if (inst.empty() || !impAlb_.id) return;
    setV3(impSh_, "camPos", cam_.position);
    setF(impSh_, "impSize", imp_[set].size);
    setF(impSh_, "impCy", imp_[set].cy);
    setF(impSh_, "impRow", float(set));
    static int dbg = std::getenv("CORERAY_IMPDEBUG") ? 1 : 0;
    setI(impSh_, "impDebug", dbg);
    DrawMeshInstanced(impQuad_, impMat_, inst.data(), int(inst.size()));
}
