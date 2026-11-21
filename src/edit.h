// Loupe - quick edits: crop, rotate, flip, straighten, exposure, contrast, saturation, warmth.
//
// Edits are a small set of parameters applied live on the GPU while you work (Direct2D effects and a
// transform), then rendered once at full resolution on the offscreen device when you save.
#pragma once
#include "common.h"
#include "offscreen.h"
#include <d2d1_1.h>

struct EditParams {
    int rot90 = 0;               // clockwise quarter turns, 0-3
    bool flipH = false, flipV = false;
    float straighten = 0;        // degrees, -45..45
    // crop in the rotated/flipped image, as fractions of its width and height
    float cropL = 0, cropT = 0, cropR = 1, cropB = 1;
    int aspect = 0;              // index into kAspects (0 = free)
    bool aspectPortrait = false;
    float exposure = 0;          // EV, -2..2
    float contrast = 0;          // -1..1
    float saturation = 0;        // -1..1
    float warmth = 0;            // -1..1

    bool HasGeometry() const { return rot90 || flipH || flipV || straighten != 0 || cropL > 0.0005f || cropT > 0.0005f || cropR < 0.9995f || cropB < 0.9995f; }
    bool HasAdjustments() const { return exposure != 0 || contrast != 0 || saturation != 0 || warmth != 0; }
    bool IsIdentity() const { return !HasGeometry() && !HasAdjustments(); }
};

struct AspectPreset { const wchar_t* label; float w, h; };   // w=h=0: free, -1: the original's
extern const AspectPreset kAspects[];
extern const int kAspectCount;

// Size of the image after the quarter turns
void OrientedSize(const EditParams& e, float w, float h, float& ow, float& oh);

// Maps the image (0..w, 0..h) into the rotated, flipped and straightened frame (0..ow, 0..oh)
D2D1_MATRIX_3X2_F EditMatrix(const EditParams& e, float w, float h);

// The largest crop with the crop's aspect, centered on the crop, that stays inside the straightened image
void FitCropInside(EditParams& e, float w, float h);
bool CropInside(const EditParams& e, float w, float h);
// Sets the crop to the biggest rectangle of the chosen aspect (or the whole image for free)
void ResetCropForAspect(EditParams& e, float w, float h);

// Builds the adjustment effect chain over `input`. Returns the output image (AddRef'd), or `input`
// AddRef'd when there's nothing to adjust. `effects` receives the created effects (to release later).
ID2D1Image* ApplyAdjustments(ID2D1DeviceContext* dc, ID2D1Image* input, const EditParams& e, std::vector<ID2D1Effect*>& effects);

// Renders the edited image at full resolution (worker thread; uses the offscreen device)
bool RenderEdited(const Pixels& src, const EditParams& e, Pixels& out, std::wstring* error);
