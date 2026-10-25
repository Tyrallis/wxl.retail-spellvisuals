// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Body glows: what a retail kit does to the unit itself (EdgeGlowEffect, ShadowyEffect,
// GradientEffect, OutlineEffect) -- Avenging Wrath's gold, Combustion's fire, Bestial Wrath's red.

#pragma once

#include <cstdint>
#include <vector>

namespace glow
{
    /// One glow, as the Workbench writes it (a `glow` line): a ramp over the body from its middle
    /// (C0) to its silhouette (C2) by a fresnel term, and a halo just outside the silhouette.
    struct Params
    {
        uint32_t fadeIn = 0, fadeOut = 0; ///< ms
        float    colors[3][3]{};
        float    alphaMiddle = 0.0f, alphaEdge = 0.0f;
        float    power = 1.0f;
        float    halo[3]{};
        float    haloPx = 0.0f, haloAlpha = 0.0f;
    };

    /// A unit glowing this frame, at a strength (its fade).
    struct Active
    {
        uint64_t      unit = 0;
        const Params* params = nullptr;
        float         strength = 1.0f;
    };

    /// Starts drawing when the runtime publishes wxl.gx; @p provider is asked, at the start of every
    /// world render, which units glow. False when glows cannot be drawn.
    bool Init(void (*provider)(std::vector<Active>& out));
    void Shutdown();
}
