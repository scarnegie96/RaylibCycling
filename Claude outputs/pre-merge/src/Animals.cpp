// Farm animals and deer: smooth lofted meshes (torso, neck + head, two-segment legs with
// hooves, tail, ears) shaded by a coat shader (fur streaks, cow patches, curly wool, pale
// belly / rump), animated with walk / trot / bound gaits, grazing and tail swishes.
#include <algorithm>
#include <cmath>
#include <functional>

#include "RenderUtil.h"
#include "Renderer.h"

using namespace rutil;

namespace {

struct Ring { Vector3 c; float rx, ry; };
// part code per vertex (see FS_ANIMAL): 0 coat, .2 dark hair, .4 hoof, .6 horn, .8 eye/nose, 1 pale
using PartFn = std::function<float(int ring, int nRings, float ang)>;

void loft(MeshBuilder& b, const std::vector<Ring>& R, int sides, const PartFn& part) {
    const int n = int(R.size());
    unsigned short base = (unsigned short)(b.v.size() / 3);
    for (int i = 0; i < n; ++i) {
        Vector3 t = Vector3Subtract(R[std::min(n - 1, i + 1)].c, R[std::max(0, i - 1)].c);
        t = Vector3Normalize(t);
        Vector3 X{1, 0, 0};
        Vector3 right = Vector3Normalize(Vector3Subtract(X, Vector3Scale(t, Vector3DotProduct(X, t))));
        Vector3 up = Vector3CrossProduct(t, right);
        // how fast the radius changes along the path tilts the normal
        float ds = std::max(1e-4f, Vector3Distance(R[std::min(n - 1, i + 1)].c, R[std::max(0, i - 1)].c));
        float drx = (R[std::min(n - 1, i + 1)].rx - R[std::max(0, i - 1)].rx) / ds;
        float dry = (R[std::min(n - 1, i + 1)].ry - R[std::max(0, i - 1)].ry) / ds;
        for (int j = 0; j <= sides; ++j) {
            float a = 2 * PI_F * j / sides;
            float ca = std::cos(a), sa = std::sin(a);
            Vector3 p = Vector3Add(R[i].c, Vector3Add(Vector3Scale(right, ca * R[i].rx), Vector3Scale(up, sa * R[i].ry)));
            Vector3 nn = Vector3Add(Vector3Scale(right, ca / std::max(1e-3f, R[i].rx)), Vector3Scale(up, sa / std::max(1e-3f, R[i].ry)));
            nn = Vector3Normalize(nn);
            float slope = (ca * ca * drx + sa * sa * dry);
            nn = Vector3Normalize(Vector3Subtract(nn, Vector3Scale(t, slope)));
            unsigned char pc = (unsigned char)std::clamp(part(i, n, a) * 255.f, 0.f, 255.f);
            b.add(p, nn, {float(j) / sides, float(i) / (n - 1)}, Color{pc, 0, 0, 255});
        }
    }
    for (int i = 0; i < n - 1; ++i)
        for (int j = 0; j < sides; ++j) {
            unsigned short a = base + i * (sides + 1) + j, a1 = a + 1, c = base + (i + 1) * (sides + 1) + j, c1 = c + 1;
            b.tri(a, a1, c);
            b.tri(a1, c1, c);
        }
}

PartFn solid(float v) { return [v](int, int, float) { return v; }; }

// leg segment hanging down from (0,0,0): radii along its length, hoof on the last `hoof` fraction
Mesh legMesh(float len, std::vector<float> radii, float hoof) {
    MeshBuilder b;
    std::vector<Ring> R;
    int n = int(radii.size());
    for (int i = 0; i < n; ++i) {
        float t = float(i) / (n - 1);
        R.push_back({{0, -len * t, 0}, radii[i], radii[i] * 1.08f});
    }
    R.front().rx *= 0.95f;
    R.push_back({{0, -len - 0.004f, 0}, 0.004f, 0.004f});  // close the end
    loft(b, R, 10, [&](int i, int nn, float) { return float(i) / (nn - 1) > 1 - hoof ? 0.4f : 0.f; });
    return b.build(true);
}

}  // namespace

void Renderer::buildAnimalMeshes() {
    // ------------------------------------------------ sheep
    {
        AnimalMeshes& m = animals_[0];
        MeshBuilder b;
        loft(b, {{{0, .66f, -.64f}, .02f, .02f}, {{0, .67f, -.59f}, .16f, .17f}, {{0, .68f, -.49f}, .27f, .27f}, {{0, .69f, -.3f}, .33f, .31f},
                 {{0, .69f, -.05f}, .35f, .32f}, {{0, .70f, .18f}, .33f, .31f}, {{0, .71f, .36f}, .27f, .28f}, {{0, .72f, .47f}, .17f, .2f}, {{0, .73f, .52f}, .03f, .03f}},
             18, solid(0));
        m.body = b.build(true);
        MeshBuilder h;
        loft(h, {{{0, -.02f, -.06f}, .12f, .14f}, {{0, .05f, .08f}, .115f, .125f}, {{0, .09f, .17f}, .09f, .105f}, {{0, .07f, .27f}, .078f, .09f},
                 {{0, .03f, .35f}, .058f, .065f}, {{0, 0, .40f}, .042f, .046f}, {{0, -.01f, .425f}, .012f, .012f}},
             14, [](int i, int, float) { return i >= 2 ? (i >= 6 ? 0.8f : 0.2f) : 0.f; });
        m.head = h.build(true);
        m.neckPivot = {0, .79f, .40f};
        m.hipX = .13f; m.hipY = .55f; m.hipZ = -.38f; m.shoulderY = .56f; m.shoulderZ = .33f;
        m.upperLen = .26f; m.lowerLen = .30f;
        m.upper = legMesh(m.upperLen, {.06f, .055f, .045f, .036f}, 0);
        m.lower = legMesh(m.lowerLen, {.03f, .026f, .024f, .023f, .027f}, .12f);
        MeshBuilder t;
        loft(t, {{{0, 0, 0}, .05f, .06f}, {{0, -.06f, -.04f}, .045f, .05f}, {{0, -.14f, -.06f}, .035f, .035f}, {{0, -.18f, -.06f}, .01f, .01f}}, 8, solid(0));
        m.tail = t.build(true);
        m.tailRoot = {0, .7f, -.6f};
        MeshBuilder e;
        loft(e, {{{0, 0, 0}, .02f, .008f}, {{.04f, 0, 0}, .03f, .01f}, {{.09f, 0, 0}, .022f, .008f}, {{.11f, 0, 0}, .004f, .004f}}, 8, solid(0.2f));
        m.ear = e.build(true);
    }
    // ------------------------------------------------ cow
    {
        AnimalMeshes& m = animals_[1];
        MeshBuilder b;
        loft(b, {{{0, 1.08f, -1.03f}, .03f, .03f}, {{0, 1.13f, -.98f}, .22f, .25f}, {{0, 1.15f, -.85f}, .31f, .35f}, {{0, 1.10f, -.55f}, .37f, .43f},
                 {{0, 1.04f, -.2f}, .41f, .47f}, {{0, 1.06f, .15f}, .40f, .45f}, {{0, 1.11f, .45f}, .36f, .43f}, {{0, 1.13f, .70f}, .29f, .37f},
                 {{0, 1.14f, .86f}, .17f, .23f}, {{0, 1.14f, .91f}, .03f, .03f}},
             22, solid(0));
        m.body = b.build(true);
        MeshBuilder h;
        loft(h, {{{0, 0, -.06f}, .2f, .27f}, {{0, -.02f, .14f}, .19f, .24f}, {{0, -.06f, .32f}, .16f, .2f}, {{0, -.11f, .47f}, .125f, .155f},
                 {{0, -.17f, .59f}, .105f, .115f}, {{0, -.2f, .65f}, .075f, .07f}, {{0, -.2f, .675f}, .02f, .02f}},
             16, [](int i, int, float) { return i >= 5 ? 0.8f : 0.f; });
        m.head = h.build(true);
        m.neckPivot = {0, 1.22f, .80f};
        m.hipX = .22f; m.hipY = .95f; m.hipZ = -.72f; m.shoulderY = .92f; m.shoulderZ = .55f;
        m.upperLen = .42f; m.lowerLen = .52f;
        m.upper = legMesh(m.upperLen, {.12f, .11f, .085f, .068f}, 0);
        m.lower = legMesh(m.lowerLen, {.058f, .05f, .047f, .05f, .056f}, .14f);
        MeshBuilder t;
        loft(t, {{{0, 0, 0}, .035f, .04f}, {{0, -.15f, -.05f}, .022f, .022f}, {{0, -.4f, -.07f}, .016f, .016f}, {{0, -.52f, -.07f}, .04f, .04f}, {{0, -.62f, -.07f}, .01f, .01f}},
             8, [](int i, int, float) { return i >= 3 ? 0.2f : 0.f; });
        m.tail = t.build(true);
        m.tailRoot = {0, 1.12f, -1.0f};
        MeshBuilder e;
        loft(e, {{{0, 0, 0}, .03f, .01f}, {{.07f, 0, 0}, .055f, .014f}, {{.15f, 0, 0}, .035f, .01f}, {{.18f, 0, 0}, .005f, .005f}}, 8, solid(0));
        m.ear = e.build(true);
    }
    // ------------------------------------------------ deer
    {
        AnimalMeshes& m = animals_[2];
        MeshBuilder b;
        auto deerPart = [](int i, int n, float a) {
            float s = std::sin(a);
            if (s < -0.55f) return 1.f;               // pale belly
            if (i <= 1 && n > 3) return 1.f;          // white rump
            return 0.f;
        };
        loft(b, {{{0, .98f, -.58f}, .02f, .02f}, {{0, 1.0f, -.55f}, .13f, .16f}, {{0, 1.0f, -.42f}, .19f, .23f}, {{0, .96f, -.2f}, .21f, .26f},
                 {{0, .96f, .05f}, .21f, .27f}, {{0, 1.0f, .28f}, .19f, .26f}, {{0, 1.04f, .42f}, .15f, .2f}, {{0, 1.06f, .5f}, .03f, .03f}},
             18, deerPart);
        m.body = b.build(true);
        MeshBuilder h;
        loft(h, {{{0, -.03f, -.03f}, .09f, .12f}, {{0, .16f, .08f}, .075f, .095f}, {{0, .36f, .16f}, .065f, .08f}, {{0, .47f, .2f}, .07f, .085f},
                 {{0, .47f, .31f}, .06f, .07f}, {{0, .44f, .40f}, .042f, .046f}, {{0, .42f, .45f}, .022f, .022f}, {{0, .415f, .465f}, .006f, .006f}},
             14, [](int i, int, float a) { return i >= 6 ? 0.8f : (std::sin(a) < -0.6f && i >= 1 && i <= 3) ? 1.f : 0.f; });
        m.head = h.build(true);
        m.neckPivot = {0, 1.10f, .40f};
        m.hipX = .1f; m.hipY = .9f; m.hipZ = -.38f; m.shoulderY = .9f; m.shoulderZ = .32f;
        m.upperLen = .38f; m.lowerLen = .52f;
        m.upper = legMesh(m.upperLen, {.065f, .058f, .042f, .034f}, 0);
        m.lower = legMesh(m.lowerLen, {.026f, .022f, .02f, .02f, .023f}, .1f);
        MeshBuilder t;
        loft(t, {{{0, 0, 0}, .04f, .05f}, {{0, -.05f, -.05f}, .045f, .05f}, {{0, -.12f, -.06f}, .02f, .02f}}, 8, solid(1.f));
        m.tail = t.build(true);
        m.tailRoot = {0, 1.0f, -.55f};
        MeshBuilder e;
        loft(e, {{{0, 0, 0}, .03f, .008f}, {{.06f, 0, 0}, .05f, .01f}, {{.13f, 0, 0}, .035f, .008f}, {{.16f, 0, 0}, .004f, .004f}}, 8, solid(0));
        m.ear = e.build(true);
    }
    // ------------------------------------------------ horse
    {
        AnimalMeshes& m = animals_[3];
        MeshBuilder b;
        loft(b, {{{0, 1.38f, -.96f}, .03f, .03f}, {{0, 1.42f, -.9f}, .22f, .26f}, {{0, 1.43f, -.75f}, .3f, .36f}, {{0, 1.37f, -.45f}, .32f, .38f},
                 {{0, 1.33f, -.1f}, .33f, .4f}, {{0, 1.36f, .25f}, .32f, .4f}, {{0, 1.41f, .55f}, .27f, .38f}, {{0, 1.45f, .72f}, .17f, .26f}, {{0, 1.47f, .78f}, .03f, .03f}},
             22, solid(0));
        m.body = b.build(true);
        MeshBuilder h;
        loft(h, {{{0, -.02f, -.04f}, .16f, .24f}, {{0, .2f, .14f}, .13f, .2f}, {{0, .4f, .29f}, .11f, .16f}, {{0, .52f, .38f}, .11f, .14f},
                 {{0, .47f, .52f}, .1f, .12f}, {{0, .37f, .66f}, .08f, .09f}, {{0, .3f, .74f}, .062f, .07f}, {{0, .28f, .77f}, .015f, .015f}},
             16, [](int i, int, float) { return i >= 6 ? 0.8f : 0.f; });
        // mane along the top of the neck
        loft(h, {{{0, .2f, -.1f}, .02f, .02f}, {{0, .3f, -.04f}, .03f, .08f}, {{0, .46f, .1f}, .03f, .08f}, {{0, .6f, .24f}, .025f, .06f}, {{0, .66f, .34f}, .01f, .01f}},
             8, solid(0.2f));
        m.head = h.build(true);
        m.neckPivot = {0, 1.55f, .6f};
        m.hipX = .18f; m.hipY = 1.22f; m.hipZ = -.62f; m.shoulderY = 1.2f; m.shoulderZ = .45f;
        m.upperLen = .52f; m.lowerLen = .68f;
        m.upper = legMesh(m.upperLen, {.13f, .115f, .08f, .065f}, 0);
        m.lower = legMesh(m.lowerLen, {.05f, .043f, .042f, .045f, .058f, .052f, .065f}, .09f);
        MeshBuilder t;
        loft(t, {{{0, 0, 0}, .06f, .07f}, {{0, -.2f, -.1f}, .07f, .07f}, {{0, -.45f, -.14f}, .06f, .055f}, {{0, -.65f, -.14f}, .035f, .03f}, {{0, -.72f, -.13f}, .01f, .01f}},
             10, solid(0.2f));
        m.tail = t.build(true);
        m.tailRoot = {0, 1.42f, -.92f};
        MeshBuilder e;
        loft(e, {{{0, 0, 0}, .028f, .012f}, {{.05f, 0, 0}, .035f, .014f}, {{.11f, 0, 0}, .015f, .008f}, {{.13f, 0, 0}, .003f, .003f}}, 8, solid(0));
        m.ear = e.build(true);
    }
}

void Renderer::drawAnimalPart(const Mesh& m, const Matrix& local, const Matrix& base, const Animal& a, bool sp) {
    (void)a;
    Matrix world = MatrixMultiply(local, base);
    if (sp) { DrawMesh(m, depthMat_, world); return; }
    DrawMesh(m, animalMat_, world);
}

void Renderer::drawAnimal(const Animal& a, bool sp) {
    if (a.kind < 0 || a.kind > 3) return;
    const AnimalMeshes& M = animals_[a.kind];
    const bool bounding = a.kind == 2 && a.fleeT >= 0;
    float moving = std::min(1.f, a.speed / 1.0f);
    float gaitHz = bounding ? 2.6f : a.speed > 2.5f ? 2.2f : 1.1f;
    float ph = a.phase * gaitHz * 2 * PI_F;
    float bob = bounding ? std::fabs(std::sin(ph * 0.5f)) * 0.32f : std::fabs(std::sin(ph)) * 0.025f * moving;
    float pitch = bounding ? std::sin(ph * 0.5f) * 0.18f : 0.f;
    Matrix base = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixScale(a.scale, a.scale, a.scale), MatrixRotateX(pitch)), MatrixRotateY(a.heading)),
                                 MatrixTranslate(a.pos.x, a.pos.y + bob * a.scale, a.pos.z));
    if (!sp) {
        auto lin = [](Color c) { return Vector3{c.r / 255.f * c.r / 255.f, c.g / 255.f * c.g / 255.f, c.b / 255.f * c.b / 255.f}; };
        rutil::setI(animalSh_, "species", a.kind);
        rutil::setV3(animalSh_, "coatA", lin(a.coat));
        rutil::setV3(animalSh_, "coatB", lin(a.coatB));
        rutil::setF(animalSh_, "patchAmt", a.patch);
        rutil::setF(animalSh_, "seed", a.seed * 10.f);
        rutil::setF(animalSh_, "sheen", a.kind == 3 ? 0.25f : a.kind == 2 ? 0.08f : 0.f);
    }
    // body breathes a little
    float breath = 1.f + 0.012f * std::sin(a.phase * 1.6f);
    drawAnimalPart(M.body, MatrixScale(1, breath, 1), base, a, sp);

    // neck + head: grazing pitch, chewing, the odd look around
    float maxPitch = a.kind == 0 ? 0.95f : a.kind == 1 ? 0.8f : a.kind == 2 ? 1.25f : 1.15f;
    float chew = a.graze > 0.5f ? std::sin(a.phase * 7.f) * 0.03f : 0.f;
    float look = (1 - a.graze) * std::sin(a.phase * 0.37f + a.seed * 9.f) * 0.35f;
    Matrix head = MatrixMultiply(MatrixMultiply(MatrixRotateX(a.graze * maxPitch + chew - (bounding ? 0.2f : 0.f)), MatrixRotateY(look)),
                                 MatrixTranslate(M.neckPivot.x, M.neckPivot.y, M.neckPivot.z));
    drawAnimalPart(M.head, head, base, a, sp);
    // ears and eyes ride on the head
    struct HeadSpot { Vector3 ear, eye; float earTilt; };
    static const HeadSpot spots[4] = {{{.06f, .13f, .2f}, {.06f, .1f, .26f}, -0.5f},
                                      {{.14f, .1f, .3f}, {.12f, .02f, .38f}, -0.2f},
                                      {{.05f, .53f, .25f}, {.055f, .5f, .32f}, 0.7f},
                                      {{.05f, .6f, .42f}, {.08f, .52f, .52f}, 1.1f}};
    const HeadSpot& hs = spots[a.kind];
    for (float sx : {-1.f, 1.f}) {
        float flick = std::sin(a.phase * 3.f + sx) > 0.97f ? 0.4f : 0.f;
        Matrix ear = MatrixMultiply(MatrixMultiply(MatrixRotateZ(hs.earTilt + flick), MatrixRotateY(sx > 0 ? -0.3f : PI_F + 0.3f)),
                                    MatrixTranslate(sx * hs.ear.x, hs.ear.y, hs.ear.z));
        drawAnimalPart(M.ear, MatrixMultiply(ear, head), base, a, sp);
        if (!sp) {
            Matrix eye = MatrixMultiply(MatrixScale(.018f, .018f, .018f), MatrixTranslate(sx * hs.eye.x, hs.eye.y, hs.eye.z));
            drawPart(sphere_, MatrixMultiply(eye, head), base, {8, 6, 6, 255}, 0.05f, 1.2f, false);
        }
        if (a.kind == 1 && !sp) {  // short horns
            Matrix horn = MatrixMultiply(MatrixMultiply(MatrixScale(.025f, .11f, .025f), MatrixRotateZ(-sx * 1.0f)), MatrixTranslate(sx * .1f, .13f, .2f));
            drawPart(cone_, MatrixMultiply(horn, head), base, {214, 204, 176, 255}, 0.4f, 0.4f, false);
        }
    }
    // legs: LH, LF, RH, RF; walk = four-beat, trot = diagonal pairs, bound = pairs
    for (int i = 0; i < 4; ++i) {
        bool hind = i % 2 == 0;
        float sx = i < 2 ? 1.f : -1.f;
        float offs;
        if (bounding) offs = hind ? 0.f : 0.9f;
        else if (a.speed > 2.5f) offs = (i == 0 || i == 3) ? 0.f : PI_F;
        else offs = float(i == 0 ? 0 : i == 1 ? 1 : i == 2 ? 2 : 3) * PI_F * 0.5f;
        float p = (bounding ? ph * 0.5f : ph) + offs;
        float amp = bounding ? 0.75f : a.speed > 2.5f ? 0.5f : 0.32f;
        float swing = std::sin(p) * amp * (bounding ? 1.f : moving);
        float lift = std::max(0.f, std::cos(p)) * moving;
        float knee = (hind ? -1.f : 1.f) * (0.15f + lift * (bounding ? 1.1f : 0.7f)) * (bounding || moving > 0.05f ? 1.f : 0.25f);
        Vector3 joint{sx * M.hipX, hind ? M.hipY : M.shoulderY, hind ? M.hipZ : M.shoulderZ};
        // keep the hooves on the ground: shorten nothing, just tilt the upper leg back a touch on the hind legs
        float rest = hind ? 0.08f : -0.05f;
        Matrix upper = MatrixMultiply(MatrixRotateX(swing + rest - knee * 0.35f), MatrixTranslate(joint.x, joint.y, joint.z));
        Matrix lower = MatrixMultiply(MatrixMultiply(MatrixRotateX(knee), MatrixTranslate(0, -M.upperLen, 0)), upper);
        drawAnimalPart(M.upper, upper, base, a, sp);
        drawAnimalPart(M.lower, lower, base, a, sp);
    }
    // tail swish
    float sw = std::sin(a.phase * (a.kind == 1 ? 1.3f : 2.2f) + a.seed * 6.f) * (a.kind == 2 ? 0.15f : 0.35f);
    Matrix tail = MatrixMultiply(MatrixMultiply(MatrixRotateZ(sw), MatrixRotateX(bounding ? -0.9f : 0.f)), MatrixTranslate(M.tailRoot.x, M.tailRoot.y, M.tailRoot.z));
    drawAnimalPart(M.tail, tail, base, a, sp);
    if (a.kind == 1 && !sp) {  // udder
        drawPart(sphere_, MatrixMultiply(MatrixScale(.13f, .1f, .16f), MatrixTranslate(0, .66f, -.45f)), base, {222, 170, 160, 255}, 0.6f, 0.2f, false);
    }
}
