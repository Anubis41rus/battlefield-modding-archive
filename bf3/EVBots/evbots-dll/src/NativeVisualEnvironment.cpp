// Evidence: visual-environment-report.md and visual-runtime-schema.json in
// D:/Games-cache/re/royale-entry-20261117. Addresses are the stock x86 zbf3 image.
#include "NativeVisualEnvironment.h"
#include "NativeVisualSettings.h"
#include "NativeBlueprint.h"
#include "InlineHook.h"
#include "VextResourceManager.h"
#include "EVBotsLog.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace evbots { namespace visual { namespace {
using Ptr=std::uintptr_t;
struct Signature { Ptr address; unsigned char bytes[16]; };
const Signature kSignatures[]={
 {0x00F77140,{0x8B,0x44,0x24,0x04,0x56,0x50,0x8B,0xF1,0xE8,0x63,0xFE,0x00,0x00,0x8B,0x0D,0x44}},
 {0x00F7A7E0,{0x56,0x8B,0xF1,0x8B,0x46,0x10,0x8B,0x8E,0x24,0x01,0x00,0x00,0x50,0x51,0x8D,0x96}},
 {0x01795CF0,{0x56,0x57,0x8B,0xF1,0xE8,0x87,0xA2,0xCD,0xFE,0xA1,0xD8,0x68,0x3B,0x02,0x8B,0x7C}},
 {0x01785A90,{0x56,0x8B,0xF1,0xE8,0xE8,0xA4,0xCE,0xFE,0x8B,0x46,0x24,0x2B,0x46,0x20,0x8B,0x54}},
 {0x01781A80,{0xC6,0x81,0xC0,0x18,0x00,0x00,0x01,0xC2,0x04,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC}},
 {0x0046FF80,{0x51,0xFF,0x15,0x0C,0xD7,0x3B,0x02,0xC3,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC}},
 {0x0046FF90,{0x51,0xFF,0x15,0x08,0xD7,0x3B,0x02,0xC3,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC}}
};
const Field kStateFields[]={
 {"visibility",Kind::Float,0},{"priority",Kind::Integer,4},
 {"overrideVisibility",Kind::Boolean,8},{"excluded",Kind::Boolean,9},
 {"stateId",Kind::Integer,12},{"activeEffectPosition",Kind::Vec3,16}
};
// Runtime component field tables follow below. Generated from decoded binding
// registrations, checked against local VU docs and independent native setters.
const Field kSkyData[]={
 {"enable",Kind::Boolean,0xCC}, // VEXT 0x1090b930
 {"skyVisibilityExponent",Kind::Float,0xB8}, // VEXT 0x1090bbf0
 {"customEnvmapAmbient",Kind::Float,0xC8}, // VEXT 0x1090b830
 {"customEnvmapScale",Kind::Float,0xC4}, // VEXT 0x1090b840
 {"customEnvmapTexture",Kind::Asset,0xC0}, // VEXT 0x1090b850
 {"skyEnvmap8BitTexScale",Kind::Float,0xBC}, // VEXT 0x1090bb00
 {"staticEnvmapScale",Kind::Float,0xA8}, // VEXT 0x1090bc00
 {"staticEnvmapTexture",Kind::Asset,0xB4}, // VEXT 0x1090bc10
 {"cloudLayer2Texture",Kind::Asset,0xB0}, // VEXT 0x1090b740
 {"cloudLayer2AlphaMul",Kind::Float,0xAC}, // VEXT 0x1090b6d0
 {"cloudLayer2AmbientLightIntensity",Kind::Float,0xA4}, // VEXT 0x1090b6f0
 {"cloudLayer2SunLightPower",Kind::Float,0xA0}, // VEXT 0x1090b730
 {"cloudLayer2SunLightIntensity",Kind::Float,0x9C}, // VEXT 0x1090b720
 {"cloudLayer2Speed",Kind::Float,0x98}, // VEXT 0x1090b710
 {"cloudLayer2Rotation",Kind::Float,0x94}, // VEXT 0x1090b700
 {"cloudLayer2TileFactor",Kind::Float,0x90}, // VEXT 0x1090b820
 {"cloudLayer2Altitude",Kind::Float,0x8C}, // VEXT 0x1090b6e0
 {"cloudLayer1Texture",Kind::Asset,0x88}, // VEXT 0x1090b5f0
 {"cloudLayer1AlphaMul",Kind::Float,0x84}, // VEXT 0x1090b5c0
 {"cloudLayer1AmbientLightIntensity",Kind::Float,0x7C}, // VEXT 0x1090b5d0
 {"cloudLayer1SunLightPower",Kind::Float,0x80}, // VEXT 0x1090b5e0
 {"cloudLayer1SunLightIntensity",Kind::Float,0x74}, // VEXT 0x108e9dd0
 {"cloudLayer1Speed",Kind::Float,0x70}, // VEXT 0x108e9cc0
 {"cloudLayer1Rotation",Kind::Float,0x6C}, // VEXT 0x108b4ff0
 {"cloudLayer1TileFactor",Kind::Float,0x68}, // VEXT 0x108b4fe0
 {"cloudLayer1Altitude",Kind::Float,0x64}, // VEXT 0x108b5070
 {"cloudLayerMaskTexture",Kind::Asset,0x60}, // VEXT 0x108e9cd0
 {"panoramicAlphaTexture",Kind::Asset,0x58}, // VEXT 0x1090b940
 {"panoramicTexture",Kind::Asset,0x54}, // VEXT 0x1090ba20
 {"panoramicRotation",Kind::Float,0x78}, // VEXT 0x108e9dc0
 {"panoramicTileFactor",Kind::Float,0x5C}, // VEXT 0x1088e2a0
 {"panoramicUVMaxY",Kind::Float,0x48}, // VEXT 0x1088e2b0
 {"panoramicUVMinY",Kind::Float,0x44}, // VEXT 0x1088e2c0
 {"panoramicUVMaxX",Kind::Float,0x40}, // VEXT 0x1088e350
 {"panoramicUVMinX",Kind::Float,0x50}, // VEXT 0x1088e4a0
 {"sunScale",Kind::Float,0x3C}, // VEXT 0x108cca00
 {"sunSize",Kind::Float,0x34}, // VEXT 0x10881990
 {"skyGradientTexture",Kind::Asset,0x30}, // VEXT 0x1090bb10
 {"brightnessScale",Kind::Float,0x38}, // VEXT 0x1090b5b0
 {"cloudLayer2Color",Kind::Vec3,0x10}, // VEXT 0x10878190
 {"cloudLayer1Color",Kind::Vec3,0x20}, // VEXT 0x10881830
 {"cloudLayerSunColor",Kind::Vec3,0x0}, // VEXT 0x108780e0
};
const Field kOutdoorLightData[]={
 {"cloudShadowEnable",Kind::Boolean,0x7D}, // VEXT 0x108e9cb0
 {"enable",Kind::Boolean,0x7C}, // VEXT 0x108e9db0
 {"translucencyDistortion",Kind::Float,0x64}, // VEXT 0x108b5070
 {"translucencyPower",Kind::Float,0x78}, // VEXT 0x108e9dc0
 {"translucencyScale",Kind::Float,0x74}, // VEXT 0x108e9dd0
 {"translucencyAmbient",Kind::Float,0x6C}, // VEXT 0x108b4ff0
 {"cloudShadowExponent",Kind::Float,0x70}, // VEXT 0x108e9cc0
 {"cloudShadowCoverage",Kind::Float,0x5C}, // VEXT 0x1088e2a0
 {"cloudShadowSize",Kind::Float,0x68}, // VEXT 0x108b4fe0
 {"cloudShadowSpeed",Kind::Vec2,0x10}, // VEXT 0x108becd0
 {"cloudShadowTexture",Kind::Asset,0x60}, // VEXT 0x108e9cd0
 {"sunShadowHeightScale",Kind::Float,0x58}, // VEXT 0x1088e340
 {"skyEnvmapShadowScale",Kind::Float,0x54}, // VEXT 0x1088e290
 {"sunSpecularScale",Kind::Float,0x50}, // VEXT 0x1088e4a0
 {"skyLightAngleFactor",Kind::Float,0x4C}, // VEXT 0x108ccca0
 {"sunRotationY",Kind::Float,0x40}, // VEXT 0x1088e350
 {"sunRotationX",Kind::Float,0x44}, // VEXT 0x1088e2c0
 {"groundColor",Kind::Vec3,0x30}, // VEXT 0x108b5080
 {"skyColor",Kind::Vec3,0x20}, // VEXT 0x10881830
 {"sunColor",Kind::Vec3,0x0}, // VEXT 0x108780e0
};
const Field kEnlightenData[]={
 {"enable",Kind::Boolean,0x71}, // VEXT 0x108b4fd0
 {"skyBoxEnable",Kind::Boolean,0x70}, // VEXT 0x108b5000
 {"skyBoxBackLightRotationY",Kind::Float,0x60}, // VEXT 0x1088e330
 {"skyBoxBackLightRotationX",Kind::Float,0x6C}, // VEXT 0x108b4ff0
 {"skyBoxBackLightColorSize",Kind::Float,0x68}, // VEXT 0x108b4fe0
 {"skyBoxSunLightColorSize",Kind::Float,0x64}, // VEXT 0x108b5070
 {"cullDistance",Kind::Float,0x54}, // VEXT 0x1088e290
 {"sunScale",Kind::Float,0x58}, // VEXT 0x1088e340
 {"bounceScale",Kind::Float,0x50}, // VEXT 0x1088e4a0
 {"skyBoxBackLightColor",Kind::Vec3,0x10}, // VEXT 0x10878190
 {"skyBoxSunLightColor",Kind::Vec3,0x40}, // VEXT 0x108b5010
 {"skyBoxGroundColor",Kind::Vec3,0x20}, // VEXT 0x10881830
 {"skyBoxSkyColor",Kind::Vec3,0x0}, // VEXT 0x108780e0
 {"terrainColor",Kind::Vec3,0x30}, // VEXT 0x108b5080
};
const Field kTonemapData[]={
 {"chromostereopsisEnable",Kind::Boolean,0x30}, // VEXT 0x108a9c20
 {"chromostereopsisOffset",Kind::Float,0x28}, // VEXT 0x10878200
 {"chromostereopsisScale",Kind::Float,0x2C}, // VEXT 0x108780d0
 {"exposureAdjustTime",Kind::Float,0x20}, // VEXT 0x10899960
 {"maxExposure",Kind::Float,0x24}, // VEXT 0x10878140
 {"minExposure",Kind::Float,0x18}, // VEXT 0x10899920
 {"middleGray",Kind::Float,0x1C}, // VEXT 0x108999b0
 {"tonemapMethod",Kind::Integer,0x14}, // VEXT 0x10925010
 {"bloomScale",Kind::Vec3,0x0}, // VEXT 0x108be940
};
const Field kColorCorrectionData[]={
 {"colorGradingEnable",Kind::Boolean,0x3C}, // VEXT 0x10881890
 {"enable",Kind::Boolean,0x3D}, // VEXT 0x10881980
 {"colorGradingTexture",Kind::Asset,0x38}, // VEXT 0x108818a0
 {"hue",Kind::Float,0x34}, // VEXT 0x10881990
 {"saturation",Kind::Vec3,0x10}, // VEXT 0x10878190
 {"contrast",Kind::Vec3,0x0}, // VEXT 0x108780e0
 {"brightness",Kind::Vec3,0x20}, // VEXT 0x10881830
};
const Field kFogData[]={
 {"heightFogEnable",Kind::Boolean,0x66}, // VEXT 0x108cccc0
 {"fogColorEnable",Kind::Boolean,0x67}, // VEXT 0x108ccc90
 {"fogGradientEnable",Kind::Boolean,0x64}, // VEXT 0x108cccb0
 {"enable",Kind::Boolean,0x65}, // VEXT 0x108cc9f0
 {"heightFogVisibilityRange",Kind::Float,0x54}, // VEXT 0x1088e290
 {"heightFogDepth",Kind::Float,0x5C}, // VEXT 0x1088e2a0
 {"heightFogAltitude",Kind::Float,0x58}, // VEXT 0x1088e340
 {"heightFogFollowCamera",Kind::Float,0x60}, // VEXT 0x1088e330
 {"transparencyFadeClamp",Kind::Float,0x50}, // VEXT 0x1088e4a0
 {"transparencyFadeEnd",Kind::Float,0x48}, // VEXT 0x1088e2b0
 {"transparencyFadeStart",Kind::Float,0x40}, // VEXT 0x1088e350
 {"fogColorEnd",Kind::Float,0x4C}, // VEXT 0x108ccca0
 {"fogColorStart",Kind::Float,0x30}, // VEXT 0x108781f0
 {"endValue",Kind::Float,0x3C}, // VEXT 0x108cca00
 {"start",Kind::Float,0x44}, // VEXT 0x1088e2c0
 {"fogDistanceMultiplier",Kind::Float,0x34}, // VEXT 0x10881990
 {"fogColorCurve",Kind::Vec4,0x10}, // VEXT 0x1088e4b0
 {"fogColor",Kind::Vec3,0x20}, // VEXT 0x108cca10
 {"curve",Kind::Vec4,0x0}, // VEXT 0x1088e360
};
const Field kWindData[]={
 {"windStrength",Kind::Float,0x8}, // VEXT 0x10899990
 {"windDirection",Kind::Float,0x4}, // VEXT 0x108999a0
};
const Field kSunFlareData[]={
 {"element5Shader",Kind::Asset,0x1AC}, // VEXT 0x10919d00
 {"element4Shader",Kind::Asset,0x190}, // VEXT 0x10919ae0
 {"element3Shader",Kind::Asset,0x1A4}, // VEXT 0x10919800
 {"element2Shader",Kind::Asset,0x188}, // VEXT 0x10919580
 {"element1Shader",Kind::Asset,0x180}, // VEXT 0x109192a0
 {"element5Enable",Kind::Boolean,0x1B0}, // VEXT 0x10919ce0
 {"element4Enable",Kind::Boolean,0x1B6}, // VEXT 0x10919ac0
 {"element3Enable",Kind::Boolean,0x1B1}, // VEXT 0x109197e0
 {"element2Enable",Kind::Boolean,0x1B2}, // VEXT 0x10919560
 {"element1Enable",Kind::Boolean,0x1B3}, // VEXT 0x10919280
 {"debugDrawOccluder",Kind::Boolean,0x1B4}, // VEXT 0x109191b0
 {"enable",Kind::Boolean,0x1B5}, // VEXT 0x10919ea0
 {"element5Size",Kind::Vec2,0xC0}, // VEXT 0x10919de0
 {"element5RayDistance",Kind::Float,0x1A0}, // VEXT 0x10919cf0
 {"element4Size",Kind::Vec2,0x120}, // VEXT 0x10919bc0
 {"element4RayDistance",Kind::Float,0x194}, // VEXT 0x10919ad0
 {"element3Size",Kind::Vec2,0xE0}, // VEXT 0x109198e0
 {"element3RayDistance",Kind::Float,0x18C}, // VEXT 0x109197f0
 {"element2Size",Kind::Vec2,0xE8}, // VEXT 0x10919660
 {"element2RayDistance",Kind::Float,0x198}, // VEXT 0x10919570
 {"element1Size",Kind::Vec2,0x70}, // VEXT 0x10919380
 {"element1RayDistance",Kind::Float,0x19C}, // VEXT 0x10919290
 {"occluderSize",Kind::Float,0x184}, // VEXT 0x10919eb0
 {"element5AlphaScreenPosCurve",Kind::Vec4,0x0}, // VEXT 0x1088e360
 {"element5AlphaOccluderCurve",Kind::Vec4,0x10}, // VEXT 0x1088e4b0
 {"element5SizeScreenPosCurve",Kind::Vec4,0x20}, // VEXT 0x1088e2d0
 {"element5SizeOccluderCurve",Kind::Vec4,0x40}, // VEXT 0x10919e40
 {"element4AlphaScreenPosCurve",Kind::Vec4,0xF0}, // VEXT 0x10919a60
 {"element4AlphaOccluderCurve",Kind::Vec4,0x140}, // VEXT 0x10919a00
 {"element4SizeScreenPosCurve",Kind::Vec4,0x110}, // VEXT 0x10919c80
 {"element4SizeOccluderCurve",Kind::Vec4,0xD0}, // VEXT 0x10919c20
 {"element3AlphaScreenPosCurve",Kind::Vec4,0x130}, // VEXT 0x10919780
 {"element3AlphaOccluderCurve",Kind::Vec4,0x170}, // VEXT 0x10919720
 {"element3SizeScreenPosCurve",Kind::Vec4,0x160}, // VEXT 0x109199a0
 {"element3SizeOccluderCurve",Kind::Vec4,0x150}, // VEXT 0x10919940
 {"element2AlphaScreenPosCurve",Kind::Vec4,0x60}, // VEXT 0x10919500
 {"element2AlphaOccluderCurve",Kind::Vec4,0x50}, // VEXT 0x109194a0
 {"element2SizeScreenPosCurve",Kind::Vec4,0x100}, // VEXT 0x109196c0
 {"element2SizeOccluderCurve",Kind::Vec4,0x30}, // VEXT 0x1088e220
 {"element1AlphaScreenPosCurve",Kind::Vec4,0xB0}, // VEXT 0x10919220
 {"element1AlphaOccluderCurve",Kind::Vec4,0xA0}, // VEXT 0x109191c0
 {"element1SizeScreenPosCurve",Kind::Vec4,0x90}, // VEXT 0x10919440
 {"element1SizeOccluderCurve",Kind::Vec4,0x80}, // VEXT 0x109193e0
};
const Field kDynamicAOData[]={
 {"enable",Kind::Boolean,0x2C}, // VEXT 0x108a3280
 {"hbaoPowerExponent",Kind::Float,0x20}, // VEXT 0x10899960
 {"hbaoMaxFootprintRadius",Kind::Float,0x28}, // VEXT 0x10878200
 {"hbaoContrast",Kind::Float,0x24}, // VEXT 0x10878140
 {"hbaoAttenuation",Kind::Float,0x18}, // VEXT 0x10899920
 {"hbaoAngleBias",Kind::Float,0x1C}, // VEXT 0x108999b0
 {"hbaoRadius",Kind::Float,0x14}, // VEXT 0x10899980
 {"ssaoMaxDistanceOuter",Kind::Float,0x4}, // VEXT 0x108999a0
 {"ssaoMaxDistanceInner",Kind::Float,0x10}, // VEXT 0x10899940
 {"ssaoRadius",Kind::Float,0xC}, // VEXT 0x108a3290
 {"ssaoFade",Kind::Float,0x8}, // VEXT 0x10899990
};
const Field kDofData[]={
 {"blurFilter",Kind::Integer,0xC}, // VEXT 0x10899930
 {"enable",Kind::Boolean,0x29}, // VEXT 0x10899970
 {"diffusionDofEnable",Kind::Boolean,0x28}, // VEXT 0x10899950
 {"diffusionDofAperture",Kind::Float,0x24}, // VEXT 0x10878140
 {"diffusionDofFocalLength",Kind::Float,0x20}, // VEXT 0x10899960
 {"scale",Kind::Float,0x1C}, // VEXT 0x108999b0
 {"blurAdd",Kind::Float,0x18}, // VEXT 0x10899920
 {"farDistanceScale",Kind::Float,0x14}, // VEXT 0x10899980
 {"blurFilterDeviation",Kind::Float,0x10}, // VEXT 0x10899940
 {"focusDistance",Kind::Float,0x8}, // VEXT 0x10899990
 {"nearDistanceScale",Kind::Float,0x4}, // VEXT 0x108999a0
};
const Field kVignetteData[]={
 {"enable",Kind::Boolean,0x2C}, // VEXT 0x108a3280
 {"opacity",Kind::Float,0x28}, // VEXT 0x10878200
 {"exponent",Kind::Float,0x24}, // VEXT 0x10878140
 {"scale",Kind::Vec2,0x0}, // VEXT 0x1092d610
 {"color",Kind::Vec3,0x10}, // VEXT 0x1085df40
};
const Field kFilmGrainData[]={
 {"texture",Kind::Asset,0x1C}, // VEXT 0x108bebf0
 {"randomEnable",Kind::Boolean,0x21}, // VEXT 0x108bebe0
 {"linearFilteringEnable",Kind::Boolean,0x20}, // VEXT 0x108bebd0
 {"enable",Kind::Boolean,0x22}, // VEXT 0x108bebc0
 {"textureScale",Kind::Vec2,0x10}, // VEXT 0x108becd0
 {"colorScale",Kind::Vec3,0x0}, // VEXT 0x108be940
};
const Field kLensScopeData[]={
 {"enable",Kind::Boolean,0x58}, // VEXT 0x108d66d0
 {"radialBlendDistanceCoefficients",Kind::Vec2,0x10}, // VEXT 0x108d66e0
 {"chromaticAberrationDisplacement2",Kind::Vec2,0x38}, // VEXT 0x108d6610
 {"chromaticAberrationDisplacement1",Kind::Vec2,0x40}, // VEXT 0x108d65b0
 {"chromaticAberrationStrengths",Kind::Vec2,0x48}, // VEXT 0x108d6670
 {"blurCenter",Kind::Vec2,0x30}, // VEXT 0x108d6550
 {"blurScale",Kind::Float,0x50}, // VEXT 0x1088e4a0
 {"chromaticAberrationColor2",Kind::Vec3,0x20}, // VEXT 0x10881830
 {"chromaticAberrationColor1",Kind::Vec3,0x0}, // VEXT 0x108780e0
};
const Field kCameraParamsData[]={
 {"sunShadowmapViewDistance",Kind::Float,0x8}, // VEXT 0x1086d8e0
 {"nearPlane",Kind::Float,0x4}, // VEXT 0x1086d8d0
 {"viewDistance",Kind::Float,0x0}, // VEXT 0x1086d8f0
};
const Field kScreenEffectData[]={
 {"angle",Kind::Float,0x24}, // VEXT 0x10878140
 {"innerFrameOpacity",Kind::Float,0x20}, // VEXT 0x10899960
 {"outerFrameOpacity",Kind::Float,0x1C}, // VEXT 0x108999b0
 {"frameWidth",Kind::Float,0x18}, // VEXT 0x10899920
 {"shader",Kind::Asset,0x14}, // VEXT 0x108fc5b0
 {"frameType",Kind::Integer,0x10}, // VEXT 0x108f3130
 {"screenEffectParams",Kind::Vec4,0x0}, // VEXT 0x108fc330
};
const Field kDamageEffectData[]={
 {"shader",Kind::Asset,0x4C}, // VEXT 0x1088e3c0
 {"debugDamage",Kind::Boolean,0x68}, // VEXT 0x1088e280
 {"endCriticalEffectHealthThreshold",Kind::Float,0x54}, // VEXT 0x1088e290
 {"startCriticalEffectHealthThreshold",Kind::Float,0x50}, // VEXT 0x1088e4a0
 {"maxOpacityDamagePercentage",Kind::Float,0x60}, // VEXT 0x1088e330
 {"minDamagePercentageThreshold",Kind::Float,0x58}, // VEXT 0x1088e340
 {"fallofTime",Kind::Float,0x5C}, // VEXT 0x1088e2a0
 {"innerFrameOpacity",Kind::Float,0x44}, // VEXT 0x1088e2c0
 {"outerFrameOpacity",Kind::Float,0x40}, // VEXT 0x1088e350
 {"frameWidth",Kind::Float,0x48}, // VEXT 0x1088e2b0
 {"rightDamage",Kind::Vec4,0x0}, // VEXT 0x1088e360
 {"bottomDamage",Kind::Vec4,0x30}, // VEXT 0x1088e220
 {"leftDamage",Kind::Vec4,0x20}, // VEXT 0x1088e2d0
 {"topDamage",Kind::Vec4,0x10}, // VEXT 0x1088e4b0
};
const Field kPlanarReflectionData[]={
 {"skyRenderEnable",Kind::Boolean,0x14}, // VEXT 0x108f3140
 {"enable",Kind::Boolean,0x15}, // VEXT 0x108f3120
 {"horizontalDeviation",Kind::Float,0x4}, // VEXT 0x108999a0
 {"horizontalBlurFilter",Kind::Integer,0x10}, // VEXT 0x108f3130
 {"verticalDeviation",Kind::Float,0xC}, // VEXT 0x108a3290
 {"verticalBlurFilter",Kind::Integer,0x0}, // VEXT 0x108f3150
 {"groundHeight",Kind::Float,0x8}, // VEXT 0x10899990
};
const Field kDynamicEnvmapData[]={
 {"enable",Kind::Boolean,0x30}, // VEXT 0x108a9c20
 {"groundColorEnvmap",Kind::Vec3,0x10}, // VEXT 0x10878190
 {"skyColorEnvmap",Kind::Vec3,0x20}, // VEXT 0x10881830
 {"keyColorEnvmap",Kind::Vec3,0x0}, // VEXT 0x108780e0
};
const Field kCharacterLightingData[]={
 {"lockToCameraDirection",Kind::Boolean,0x34}, // VEXT 0x10878180
 {"firstPersonEnable",Kind::Boolean,0x35}, // VEXT 0x10878170
 {"characterLightEnable",Kind::Boolean,0x36}, // VEXT 0x10878150
 {"topLightDirY",Kind::Float,0x28}, // VEXT 0x10878200
 {"topLightDirX",Kind::Float,0x30}, // VEXT 0x108781f0
 {"blendFactor",Kind::Float,0x2C}, // VEXT 0x108780d0
 {"characterLightingMode",Kind::Integer,0x20}, // VEXT 0x10878160
 {"cameraUpRotation",Kind::Float,0x24}, // VEXT 0x10878140
 {"bottomLight",Kind::Vec3,0x0}, // VEXT 0x108780e0
 {"topLight",Kind::Vec3,0x10}, // VEXT 0x10878190
};
const Field kMotionBlurData[]={
 {"motionBlurEnable",Kind::Boolean,0xC}, // VEXT 0x108dd110
 {"motionBlurCutoffRadius",Kind::Float,0x8}, // VEXT 0x10899990
 {"cutoffGradientScale",Kind::Float,0x4}, // VEXT 0x108999a0
};
const Field kDebugData[]={
 {"debugTexture",Kind::Asset,0x4}, // VEXT 0x10940d30
 {"enable",Kind::Boolean,0x9}, // VEXT 0x10940e10
 {"fullscreen",Kind::Boolean,0x8}, // VEXT 0x10940e20
};
const Component kComponents[]={
 {"sky",0x30,kSkyData,static_cast<unsigned>(sizeof(kSkyData)/sizeof(kSkyData[0]))},
 {"outdoorLight",0x20,kOutdoorLightData,static_cast<unsigned>(sizeof(kOutdoorLightData)/sizeof(kOutdoorLightData[0]))},
 {"enlighten",0x24,kEnlightenData,static_cast<unsigned>(sizeof(kEnlightenData)/sizeof(kEnlightenData[0]))},
 {"tonemap",0x28,kTonemapData,static_cast<unsigned>(sizeof(kTonemapData)/sizeof(kTonemapData[0]))},
 {"colorCorrection",0x2C,kColorCorrectionData,static_cast<unsigned>(sizeof(kColorCorrectionData)/sizeof(kColorCorrectionData[0]))},
 {"fog",0x34,kFogData,static_cast<unsigned>(sizeof(kFogData)/sizeof(kFogData[0]))},
 {"wind",0x38,kWindData,static_cast<unsigned>(sizeof(kWindData)/sizeof(kWindData[0]))},
 {"sunFlare",0x3C,kSunFlareData,static_cast<unsigned>(sizeof(kSunFlareData)/sizeof(kSunFlareData[0]))},
 {"dynamicAO",0x40,kDynamicAOData,static_cast<unsigned>(sizeof(kDynamicAOData)/sizeof(kDynamicAOData[0]))},
 {"dof",0x44,kDofData,static_cast<unsigned>(sizeof(kDofData)/sizeof(kDofData[0]))},
 {"vignette",0x48,kVignetteData,static_cast<unsigned>(sizeof(kVignetteData)/sizeof(kVignetteData[0]))},
 {"filmGrain",0x4C,kFilmGrainData,static_cast<unsigned>(sizeof(kFilmGrainData)/sizeof(kFilmGrainData[0]))},
 {"lensScope",0x50,kLensScopeData,static_cast<unsigned>(sizeof(kLensScopeData)/sizeof(kLensScopeData[0]))},
 {"cameraParams",0x54,kCameraParamsData,static_cast<unsigned>(sizeof(kCameraParamsData)/sizeof(kCameraParamsData[0]))},
 {"screenEffect",0x58,kScreenEffectData,static_cast<unsigned>(sizeof(kScreenEffectData)/sizeof(kScreenEffectData[0]))},
 {"damageEffect",0x5C,kDamageEffectData,static_cast<unsigned>(sizeof(kDamageEffectData)/sizeof(kDamageEffectData[0]))},
 {"planarReflection",0x60,kPlanarReflectionData,static_cast<unsigned>(sizeof(kPlanarReflectionData)/sizeof(kPlanarReflectionData[0]))},
 {"dynamicEnvmap",0x64,kDynamicEnvmapData,static_cast<unsigned>(sizeof(kDynamicEnvmapData)/sizeof(kDynamicEnvmapData[0]))},
 {"characterLighting",0x68,kCharacterLightingData,static_cast<unsigned>(sizeof(kCharacterLightingData)/sizeof(kCharacterLightingData[0]))},
 {"motionBlur",0x6C,kMotionBlurData,static_cast<unsigned>(sizeof(kMotionBlurData)/sizeof(kMotionBlurData[0]))},
 {"debug",0xB0,kDebugData,static_cast<unsigned>(sizeof(kDebugData)/sizeof(kDebugData[0]))},
};
struct Record { int id; std::uint64_t generation; std::string name; };
std::unordered_map<Ptr,Record> g_states;
SRWLOCK g_records=SRWLOCK_INIT;
std::uint64_t g_generation=1;
InlineHook g_initHook,g_destroyHook;
using InitFn=void(__fastcall*)(void*,void*,void*);
using DestroyFn=void(__fastcall*)(void*,void*);
InitFn g_init=nullptr;
DestroyFn g_destroy=nullptr;
Ptr g_base=0;
int g_ready=0;
bool Fail(const char** error,const char* message) { if(error)*error=message;return false; }
Ptr Va(Ptr address) { return g_base+address-0x400000; }
Ptr U32(Ptr address) { return *reinterpret_cast<const std::uint32_t*>(address); }
bool Disabled() { char value[4]={};return EnvGetCached("EVBOTS_VISUAL_ENVIRONMENT",value,sizeof(value))==1 && value[0]=='0'; }
bool SignatureMatches(const Signature& signature) {
 __try { return std::memcmp(reinterpret_cast<const void*>(Va(signature.address)),signature.bytes,16)==0; }
 __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Capture(Ptr entity,Ptr params,int* id,char* name,std::size_t capacity) {
 __try {
  *id=*reinterpret_cast<const int*>(entity+0x3C);
  const Ptr blueprint=U32(params+0x80);
  const char* source=blueprint?reinterpret_cast<const char*>(U32(blueprint+8)):"EffectEntity";
  if(!source)return false;
  for(std::size_t i=0;i<capacity;++i) { name[i]=source[i];if(!source[i])return true; }
 } __except(EXCEPTION_EXECUTE_HANDLER) { }
 return false;
}
void __fastcall OnInit(void* entity,void*,void* params) {
 g_init(entity,nullptr,params);
 int id=0;char name[1024]={};
 if(!Capture(reinterpret_cast<Ptr>(entity),reinterpret_cast<Ptr>(params),&id,name,sizeof(name))) {
  LOG_ERR("VisualEnvironment: native init name could not be captured");return;
 }
 AcquireSRWLockExclusive(&g_records);
 g_states[reinterpret_cast<Ptr>(entity)+0x30]={id,g_generation++,name};
 ReleaseSRWLockExclusive(&g_records);
 visualsettings::OnStateCreated(reinterpret_cast<Ptr>(entity)+0x30,name);
}
void __fastcall OnDestroy(void* entity,void*) {
 AcquireSRWLockExclusive(&g_records);
 g_states.erase(reinterpret_cast<Ptr>(entity)+0x30);
 ReleaseSRWLockExclusive(&g_records);
 g_destroy(entity,nullptr);
}
bool Begin(Ptr* manager,const char** error) {
 if(error)*error=nullptr;
 if(g_ready!=1 || Disabled())return Fail(error,"native visual environment tracking is not enabled");
 if(!blueprint::IsClient() || !blueprint::EngineFrameActive())return Fail(error,"visual environment access requires the client engine frame");
 __try {
  *manager=U32(Va(0x023B6844));
  if(!*manager)return Fail(error,"native visual environment manager is not available");
  EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(*manager));
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { return Fail(error,"native visual environment manager is not readable"); }
}
void End(Ptr manager) { LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(manager)); }
bool Span(Ptr manager,Ptr* begin,Ptr* end) {
 __try {
  *begin=U32(manager+0x20);*end=U32(manager+0x24);
  return *end>=*begin && ((*end-*begin)&3)==0 && *end-*begin<=4*65536 && (*begin || !*end);
 } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Valid(const Handle& h,Ptr manager) {
 if(h.manager!=manager)return false;
 bool tracked=false;
 AcquireSRWLockShared(&g_records);
 const auto found=g_states.find(h.state);
 if(found!=g_states.end())tracked=found->second.generation==h.generation && found->second.id==h.id;
 ReleaseSRWLockShared(&g_records);
 if(!tracked)return false;
 __try {
  Ptr begin=0,end=0;if(!Span(manager,&begin,&end))return false;
  for(Ptr at=begin;at<end;at+=4)if(U32(at)==h.state)return *reinterpret_cast<const int*>(h.state+12)==h.id;
 } __except(EXCEPTION_EXECUTE_HANDLER) { }
 return false;
}
bool RawRead(Ptr address,Kind kind,Value* value) {
 __try {
  switch(kind) {
   case Kind::Boolean:value->integer=*reinterpret_cast<const unsigned char*>(address)!=0;break;
   case Kind::Integer:value->integer=*reinterpret_cast<const int*>(address);break;
   case Kind::Asset:value->asset=U32(address);break;
   default: {
    const unsigned count=kind==Kind::Vec4?4:kind==Kind::Vec3?3:kind==Kind::Vec2?2:1;
    std::memcpy(value->number,reinterpret_cast<const void*>(address),count*sizeof(float));break;
   }
  }
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RawWrite(Ptr address,Kind kind,const Value& value) {
 __try {
  if(kind==Kind::Boolean)*reinterpret_cast<unsigned char*>(address)=value.integer?1:0;
  else if(kind==Kind::Integer)*reinterpret_cast<int*>(address)=value.integer;
  else if(kind==Kind::Float)*reinterpret_cast<float*>(address)=value.number[0];
  else if(kind==Kind::Vec2 || kind==Kind::Vec3 || kind==Kind::Vec4)
   std::memcpy(reinterpret_cast<void*>(address),value.number,(kind==Kind::Vec2?2:kind==Kind::Vec3?3:4)*sizeof(float));
  else return false;
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ComponentAddress(const Handle& h,const Component* component,Ptr* address) {
 __try { *address=component?U32(h.state+component->stateOffset):h.state;return true; }
 __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool Initialize(const char** error) {
 if(error)*error=nullptr;
 if(g_ready==1)return true;
 if(g_ready<0)return Fail(error,"native visual environment initialization previously failed");
 if(!blueprint::IsClient() || Disabled())return Fail(error,"native visual environment is client-only or disabled");
 g_base=reinterpret_cast<Ptr>(GetModuleHandleA(nullptr));
 bool matches=true;
 for(const auto& signature:kSignatures) {
  const bool match=SignatureMatches(signature);matches=matches && match;
  LOG_INFO("VisualEnvironment: signature %08X %s",static_cast<unsigned>(signature.address),match?"PASS":"FAIL");
 }
 if(!matches) {g_ready=-1;return Fail(error,"native visual environment signature mismatch; no hooks installed");}
 if(g_initHook.Install(reinterpret_cast<void*>(Va(kSignatures[0].address)),reinterpret_cast<void*>(&OnInit),"VisualEnvironment.Init")!=HookResult::Ok) {
  g_ready=-1;return Fail(error,"native visual environment init hook failed");
 }
 g_init=reinterpret_cast<InitFn>(g_initHook.Trampoline());
 if(g_destroyHook.Install(reinterpret_cast<void*>(Va(kSignatures[1].address)),reinterpret_cast<void*>(&OnDestroy),"VisualEnvironment.Destroy")!=HookResult::Ok) {
  g_initHook.Remove();g_ready=-1;return Fail(error,"native visual environment destroy hook failed; init hook rolled back");
 }
 g_destroy=reinterpret_cast<DestroyFn>(g_destroyHook.Trampoline());
 g_ready=1;
 LOG_INFO("VisualEnvironment: native tracking enabled, %u runtime component types",static_cast<unsigned>(sizeof(kComponents)/sizeof(kComponents[0])));
 return true;
}
bool GetStates(std::vector<State>* states,const char** error) {
 if(!states)return Fail(error,"missing visual states output");
 states->clear();Ptr manager=0;if(!Begin(&manager,error))return false;
 Ptr begin=0,end=0;bool valid=Span(manager,&begin,&end);
 if(valid) {
  AcquireSRWLockShared(&g_records);
  for(Ptr at=begin;at<end && valid;at+=4) {
   Value raw;valid=RawRead(at,Kind::Asset,&raw);if(!valid)break;
   const auto found=g_states.find(raw.asset);
   if(found==g_states.end()) {valid=false;break;}
   states->push_back({{manager,raw.asset,found->second.generation,found->second.id},found->second.name});
  }
  ReleaseSRWLockShared(&g_records);
 }
 End(manager);
 if(!valid) {states->clear();return Fail(error,"native visual state list is invalid or contains an untracked entity");}
 // VEXT 10C103BA erases with memmove; native 1785A90 swaps the last
 // element. Preserve VEXT insertion order, including after entity removal.
 std::sort(states->begin(),states->end(),[](const State& a,const State& b){return a.handle.generation<b.handle.generation;});
 return true;
}
bool GetDirty(bool* dirty,const char** error) {
 Ptr manager=0;if(!dirty || !Begin(&manager,error))return false;
 Value value;const bool ok=RawRead(manager+0x18C0,Kind::Boolean,&value);End(manager);
 if(!ok)return Fail(error,"native visual dirty flag is not readable");
 *dirty=value.integer!=0;return true;
}
bool SetDirty(bool dirty,const char** error) {
 Ptr manager=0;if(!Begin(&manager,error))return false;
 Value value;value.integer=dirty?1:0;const bool ok=RawWrite(manager+0x18C0,Kind::Boolean,value);End(manager);
 return ok?true:Fail(error,"native visual dirty flag is not writable");
}
const Field* StateField(const char* name) { for(const auto& f:kStateFields)if(std::strcmp(name,f.name)==0)return &f;return nullptr; }
const Component* FindComponent(const char* name) { for(const auto& c:kComponents)if(std::strcmp(name,c.name)==0)return &c;return nullptr; }
const Field* FindField(const Component* component,const char* name) { for(unsigned i=0;i<component->count;++i)if(std::strcmp(name,component->fields[i].name)==0)return &component->fields[i];return nullptr; }
bool HasComponent(const Handle& state,const Component* component,bool* present,const char** error) {
 Ptr manager=0;if(!Begin(&manager,error))return false;
 Ptr address=0;const bool ok=Valid(state,manager) && ComponentAddress(state,component,&address);End(manager);
 if(!ok)return Fail(error,"visual state is no longer alive");
 *present=address!=0;return true;
}
bool Read(const Handle& state,const Component* component,const Field* field,Value* value,const char** error) {
 Ptr manager=0;if(!Begin(&manager,error))return false;
 Ptr address=0;bool ok=Valid(state,manager) && ComponentAddress(state,component,&address) && address && RawRead(address+field->offset,field->kind,value);
 if(ok && field->kind==Kind::Asset && value->asset)ok=ResourceRetainLive(static_cast<unsigned long>(value->asset));
 End(manager);
 return ok?true:Fail(error,"visual state/component is no longer alive or readable");
}
bool WriteState(const Handle& state,const Field* field,const Value& value,const char** error) {
 if(field->offset==12)return Fail(error,"stateId is read-only");
 if(field->kind==Kind::Float || field->kind==Kind::Vec3)for(float f:value.number)if(!std::isfinite(f))return Fail(error,"visual state value is not finite");
 Ptr manager=0;if(!Begin(&manager,error))return false;
 const bool ok=Valid(state,manager) && RawWrite(state.state+field->offset,field->kind,value);End(manager);
 return ok?true:Fail(error,"visual state is no longer alive or writable");
}
bool WriteComponent(const Handle& state,const Component* component,const Field* field,const Value& value,const char** error) {
 if(field->kind==Kind::Asset)return Fail(error,"runtime asset replacement is not implemented");
 for(float f:value.number)if(!std::isfinite(f))return Fail(error,"visual component value is not finite");
 Ptr manager=0;if(!Begin(&manager,error))return false;
 Ptr address=0;const bool ok=Valid(state,manager) && ComponentAddress(state,component,&address) && address && RawWrite(address+field->offset,field->kind,value);
 End(manager);return ok?true:Fail(error,"visual component is no longer alive or writable");
}
bool ReadSun(unsigned axis,float* value,const char** error) {
 if(axis>1)return Fail(error,"invalid sun rotation axis");
 Ptr manager=0;if(!Begin(&manager,error))return false;
 Value raw;const bool ok=RawRead(manager+0x18B0+axis*4,Kind::Float,&raw);End(manager);
 if(ok)*value=raw.number[0];
 return ok?true:Fail(error,"sun rotation is not readable");
}
bool WriteSun(unsigned axis,float value,const char** error) {
 if(axis>1 || !std::isfinite(value))return Fail(error,"invalid sun rotation value");
 Ptr manager=0;if(!Begin(&manager,error))return false;
 Value raw;raw.number[0]=value;const bool ok=RawWrite(manager+0x18B0+axis*4,Kind::Float,raw);End(manager);
 return ok?true:Fail(error,"sun rotation is not writable");
}
} }
