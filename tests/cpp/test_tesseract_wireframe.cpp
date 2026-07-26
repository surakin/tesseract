#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tk/tesseract_wireframe.h"

#include <cmath>

using tk::Point;

namespace
{

float dist(Point a, Point b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

TEST_CASE("tesseract_wireframe_edges produces exactly 32 non-degenerate edges",
          "[tk][tesseract]")
{
    const auto edges =
        tk::tesseract_wireframe_edges(0.3f, Point{0, 0}, 100.0f);
    CHECK(edges.size() == 32);

    for (const auto& e : edges)
    {
        CHECK(dist(e.a, e.b) > 0.01f); // endpoints must differ
    }
}

TEST_CASE("tesseract_wireframe_edges never produces NaN/Inf coordinates",
          "[tk][tesseract]")
{
    for (float phase : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.99f})
    {
        const auto edges =
            tk::tesseract_wireframe_edges(phase, Point{50, 50}, 30.0f);
        for (const auto& e : edges)
        {
            CHECK(std::isfinite(e.a.x));
            CHECK(std::isfinite(e.a.y));
            CHECK(std::isfinite(e.b.x));
            CHECK(std::isfinite(e.b.y));
        }
    }
}

TEST_CASE("tesseract_wireframe_edges actually rotates over phase",
          "[tk][tesseract]")
{
    const auto edges0 = tk::tesseract_wireframe_edges(0.0f, Point{0, 0}, 10.0f);
    const auto edges5 = tk::tesseract_wireframe_edges(0.5f, Point{0, 0}, 10.0f);

    bool any_different = false;
    for (std::size_t i = 0; i < edges0.size(); ++i)
    {
        if (dist(edges0[i].a, edges5[i].a) > 0.01f ||
            dist(edges0[i].b, edges5[i].b) > 0.01f)
        {
            any_different = true;
            break;
        }
    }
    CHECK(any_different);
}

TEST_CASE("tesseract_wireframe_edges matches a hand-computed vertex at phase 0",
          "[tk][tesseract]")
{
    // At phase01 = 0 the rotation angle is 0, so it's the identity — easy to
    // hand-verify. Vertex 0 (all coordinates -1) is symmetric under the
    // double-perspective projection, so x2 == y2 exactly:
    //   k4 = 1 / (2.2 - (-1))        = 1 / 3.2      = 0.3125
    //   z3 = -1 * k4                 = -0.3125
    //   k3 = 1 / (2.2 - (-0.3125))   = 1 / 2.5125    ≈ 0.397913
    //   x2 = x3 * k3 = (-1 * k4) * k3 ≈ -0.124348
    const auto edges =
        tk::tesseract_wireframe_edges(0.0f, Point{0, 0}, 1.0f);

    // Vertex 0 connects to vertices 1, 2, 4, 8 (each differs by one bit);
    // any of those edges' endpoint touching vertex 0 gives the same point.
    // Find an edge endpoint at (-0.124348, -0.124348) within tolerance.
    bool found = false;
    for (const auto& e : edges)
    {
        for (Point p : {e.a, e.b})
        {
            if (std::abs(p.x - (-0.124348f)) < 0.001f &&
                std::abs(p.y - (-0.124348f)) < 0.001f)
            {
                found = true;
            }
        }
    }
    CHECK(found);
}

TEST_CASE("tesseract_wireframe_edges respects center and scale",
          "[tk][tesseract]")
{
    const Point center{20, 20};
    const auto small = tk::tesseract_wireframe_edges(0.2f, center, 10.0f);
    const auto big   = tk::tesseract_wireframe_edges(0.2f, center, 20.0f);

    float small_sum = 0.0f;
    float big_sum   = 0.0f;
    for (std::size_t i = 0; i < small.size(); ++i)
    {
        small_sum += dist(small[i].a, center) + dist(small[i].b, center);
        big_sum += dist(big[i].a, center) + dist(big[i].b, center);
    }
    // Doubling scale should roughly double the average spread from center.
    const float ratio = big_sum / small_sum;
    CHECK(ratio > 1.8f);
    CHECK(ratio < 2.2f);
}

TEST_CASE("tesseract_wireframe_edges stays within a conservative radius bound",
          "[tk][tesseract]")
{
    const Point center{0, 0};
    const float scale = 50.0f;
    for (float phase : {0.0f, 0.1f, 0.25f, 0.37f, 0.5f, 0.63f, 0.75f, 0.9f})
    {
        const auto edges = tk::tesseract_wireframe_edges(phase, center, scale);
        for (const auto& e : edges)
        {
            CHECK(dist(e.a, center) < 1.2f * scale);
            CHECK(dist(e.b, center) < 1.2f * scale);
        }
    }
}
