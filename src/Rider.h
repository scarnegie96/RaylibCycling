#pragma once
// To-scale (1.78 m) human rider: one continuous mesh skinned to a 17-bone skeleton.
// Each frame the skeleton is posed on the bike by IK (feet on the pedals at the real crank
// angle, hands on the hoods or drops, torso from saddle to bars) and the mesh is skinned
// on the CPU (a few thousand vertices), then drawn with the kit shader.
#include <vector>

#include "raylib.h"

// Bike wheel: 700x25c (ETRTO 622 bead + 2 x 25 mm) is 336 mm free radius; under load the
// tyre squashes ~2 mm, so the axle sits (and the wheel rolls) at 334 mm. The wheels are
// drawn and spun with these, so tread speed = ground speed (2.099 m per revolution).
constexpr float WHEEL_FREE_R = 0.336f;
constexpr float WHEEL_ROLL_R = 0.334f;

struct RiderPose {
    double crank = 0;    // radians, 0 = right pedal at the bottom, increasing = pedalling forward
    float stand = 0;     // 0 seated .. 1 out of the saddle
    float tuck = 0;      // 0 hoods .. 1 drops
    float effort = 0;    // body rock
    float look = 0;      // head turn (radians)
};

class RiderRig {
public:
    enum Bone { Pelvis, Spine, Chest, Neck, Head, ThighL, ShinL, FootL, ThighR, ShinR, FootR, UpperL, ForeL, HandL, UpperR, ForeR, HandR, COUNT };
    void build();
    void unload();
    void pose(const RiderPose& p);   // poses the skeleton and skins the mesh (bike-local space)
    const Mesh& mesh() const { return mesh_; }
    Matrix boneMatrix(Bone b) const { return skin_[b]; }  // bind space -> posed (bike-local)
    // pedal position in bike-local space for a crank angle and side (0 = right, 1 = left)
    static Vector3 pedal(double crank, int side);

private:
    Vector3 bindHead_[COUNT]{}, bindTail_[COUNT]{};
    Matrix skin_[COUNT]{};
    std::vector<float> bindPos_, bindNrm_, outPos_, outNrm_;
    std::vector<unsigned char> b0_, b1_;
    std::vector<float> w_;
    Mesh mesh_{};
    bool built_ = false;
};
