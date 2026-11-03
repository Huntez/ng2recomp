// ng2 - Ninja Gaiden II static recompilation
//
// Aspect Ratio (aspect.cpp).

#pragma once

#include <cstdint>

namespace ng2::aspect {

// The game's 16:9 float, which the camera's projection is built with.
constexpr uint32_t kConstantAddress = 0x8228EBFC;

// The chosen aspect: 0 16:9, 1 16:10, 2 21:9, 3 32:9 (out of range: 0).
uint32_t AspectIndex();

// The aspect the picture is shown at (16:9 unless set otherwise).
float Aspect();

// The gameplay HUD was drawn in the last 2D pass.
bool HudShown();

// How much 2D is squeezed along x and y (1: not), about the centre of the
// game's 1280x720 space, for the presenter to stretch back.
float SqueezeX();
float SqueezeY();

// The constant's page was made writable (else the 3D stays 16:9).
void SetConstantWritable(bool writable);

}  // namespace ng2::aspect
