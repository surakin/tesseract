#pragma once

#include "tk/canvas.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace tk
{

struct LineSegment
{
    Point a, b;
};

// The 32 edges of a rotating 4D hypercube (tesseract), projected to 2D, centered at
// `center` with projected radius approximately `scale`. `phase01` in [0,1) drives a
// simultaneous XZ-plane + YW-plane rotation (a Clifford/isoclinic rotation) at the
// same angular rate. Deliberately NOT an XY-plane rotation: pairing X with Y would
// rotate both screen axes together as a single rigid in-plane spin (every vertex
// keeping the same depth/scale relative to the others), which reads as a flat
// clock-hand spin rather than a tumbling 3D/4D shape. Pairing each screen axis (X,
// Y) with a depth axis (Z, W) instead means horizontal and vertical motion are each
// coupled to a perspective-divide term, so vertices visibly grow/shrink as they
// "tumble" through the projection — the actual tesseract look. Pure geometry, no
// Canvas dependency, so it's independently testable.
inline std::array<LineSegment, 32> tesseract_wireframe_edges(float phase01,
                                                              Point center,
                                                              float scale)
{
    // 16 vertices of a unit hypercube: bit i of the vertex index selects +1/-1
    // for coordinate i (x=bit0, y=bit1, z=bit2, w=bit3).
    struct Vec4 { float x, y, z, w; };
    std::array<Vec4, 16> verts;
    for (int i = 0; i < 16; ++i)
    {
        verts[static_cast<std::size_t>(i)] = {
            (i & 1) ? 1.0f : -1.0f,
            (i & 2) ? 1.0f : -1.0f,
            (i & 4) ? 1.0f : -1.0f,
            (i & 8) ? 1.0f : -1.0f,
        };
    }

    const float angle = phase01 * 2.0f * 3.14159265f;
    const float ca = std::cos(angle);
    const float sa = std::sin(angle);

    // Perspective-divide constants: rotated coordinates never exceed ±√2 in
    // magnitude (a 2D rotation preserves a pair's norm, and vertices start at
    // ±1), so with d4 = 2.2 the first divisor stays in [0.786, 3.614] — always
    // safely positive. The second-stage divisor is tighter still (z3 is
    // already once-projected) and stays comfortably positive too.
    constexpr float kDist4 = 2.2f;
    constexpr float kDist3 = 2.2f;

    std::array<Point, 16> projected;
    for (int i = 0; i < 16; ++i)
    {
        const auto& v = verts[static_cast<std::size_t>(i)];

        // Simultaneous rotation in the XZ-plane and the YW-plane (isoclinic —
        // see the file-level comment for why not XY+ZW).
        const float x1 = v.x * ca - v.z * sa;
        const float z1 = v.x * sa + v.z * ca;
        const float y1 = v.y * ca - v.w * sa;
        const float w1 = v.y * sa + v.w * ca;

        // 4D -> 3D perspective divide.
        const float k4 = 1.0f / (kDist4 - w1);
        const float x3 = x1 * k4;
        const float y3 = y1 * k4;
        const float z3 = z1 * k4;

        // 3D -> 2D perspective divide.
        const float k3 = 1.0f / (kDist3 - z3);
        const float x2 = x3 * k3;
        const float y2 = y3 * k3;

        projected[static_cast<std::size_t>(i)] =
            Point{center.x + x2 * scale, center.y + y2 * scale};
    }

    // 32 edges: vertex pairs (i, j), i < j, that differ in exactly one bit —
    // the standard hypercube graph (16 vertices * 4 neighbours / 2 = 32).
    std::array<LineSegment, 32> edges{};
    std::size_t n = 0;
    for (int i = 0; i < 16; ++i)
    {
        for (int j = i + 1; j < 16; ++j)
        {
            const int diff = i ^ j;
            const int bits = (diff & 1) + ((diff >> 1) & 1) +
                             ((diff >> 2) & 1) + ((diff >> 3) & 1);
            if (bits == 1)
            {
                edges[n++] = {projected[static_cast<std::size_t>(i)],
                             projected[static_cast<std::size_t>(j)]};
            }
        }
    }

    return edges;
}

} // namespace tk
