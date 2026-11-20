// Unit test for RSX_AA_A2C=hashed / RSX_AA_A2C_MIPSCALE (third_party/rpcs3/rpcs3/Emu/RSX/VK/VKR2A2C.h, the CPU mirror of
// the GLSL in RSXFragmentPrologue.glsl / RSXFragmentTextureOps.glsl): env parsing, alpha clamp, unbiased coverage,
// stability (no frame term, camera-independent), cell scale following the derivative, degenerate fallback.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/VK/VKR2A2C.h"

#include <cstdio>
#include <random>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

using namespace vk::r2a2c;

int main()
{
	// Parser: unset = stable (default), hashed aliases, unknown = noise (as pdq_a2c_parse)
	CHECK(parse_mode(nullptr) == a2c_mode::stable);
	CHECK(parse_mode("") == a2c_mode::stable);
	CHECK(parse_mode("hashed") == a2c_mode::hashed);
	CHECK(parse_mode("HASH") == a2c_mode::hashed);
	CHECK(parse_mode("2") == a2c_mode::hashed);
	CHECK(parse_mode("stable") == a2c_mode::stable);
	CHECK(parse_mode("noise") == a2c_mode::noise);
	CHECK(parse_mode("0") == a2c_mode::noise);
	CHECK(parse_mode("hashedx") == a2c_mode::noise);
	CHECK(parse_mipscale(nullptr) == 0.f);
	CHECK(parse_mipscale("0") == 0.f);
	CHECK(parse_mipscale("-1") == 0.f);
	CHECK(parse_mipscale("abc") == 0.f);
	CHECK(parse_mipscale("0.25") == 0.25f);
	CHECK(parse_mipscale("99") == 4.f);

	std::mt19937 rng(1234);
	std::uniform_real_distribution<float> pos(-500.f, 500.f), lg(-12.f, 4.f);

	// Clamp: alpha 1 always passes, alpha 0 always fails, threshold within [1e-6, 1]
	int n_bad = 0;
	for (int i = 0; i < 200000; ++i)
	{
		const float p[3] = { pos(rng), pos(rng), pos(rng) };
		const float t = hashed_threshold(p, std::exp2(lg(rng)));
		if (!(t >= 1e-6f && t <= 1.f) || !hashed_passes(1.f, t) || hashed_passes(0.f, t)) ++n_bad;
	}
	CHECK(n_bad == 0);

	// Unbiased: the fraction of passing fragments matches alpha (CDF correction keeps the threshold uniform,
	// including fractional scales between two power-of-two cell sizes)
	for (const float alpha : { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f })
	{
		int pass = 0;
		const int n = 200000;
		for (int i = 0; i < n; ++i)
		{
			const float p[3] = { pos(rng), pos(rng), pos(rng) };
			pass += hashed_passes(alpha, hashed_threshold(p, std::exp2(lg(rng)))) ? 1 : 0;
		}
		const float cov = float(pass) / n;
		std::printf("alpha %.1f -> coverage %.4f\n", alpha, cov);
		CHECK(cov > alpha - 0.01f && cov < alpha + 0.01f);
	}

	// Stable: the same surface point at the same pixel footprint gets the same threshold every frame (no frame-index
	// term) and wherever the camera is (the input is the object-space position, not gl_FragCoord)
	{
		const float p[3] = { 12.375f, -3.5f, 40.125f };
		const float t0 = hashed_threshold(p, 0.02f);
		for (int f = 0; f < 64; ++f) CHECK(hashed_threshold(p, 0.02f) == t0);
	}

	// Cells about one pixel wide: points within one cell (power-of-two scale, no blend) share a threshold,
	// and the neighbouring cell differs for most points
	{
		const float d = 1.f / 64.f; // pix_scale = 64 exactly -> lerp 0
		int same = 0, diff = 0;
		for (int i = 0; i < 2000; ++i)
		{
			const float base[3] = { std::floor(pos(rng) * 64.f) / 64.f, std::floor(pos(rng) * 64.f) / 64.f, std::floor(pos(rng) * 64.f) / 64.f };
			const float in_cell[3] = { base[0] + 0.3f / 64.f, base[1] + 0.6f / 64.f, base[2] + 0.9f / 64.f };
			const float next[3] = { base[0] + 1.3f / 64.f, base[1], base[2] };
			same += hashed_threshold(base, d) == hashed_threshold(in_cell, d) ? 1 : 0;
			diff += hashed_threshold(base, d) != hashed_threshold(next, d) ? 1 : 0;
		}
		CHECK(same == 2000);
		CHECK(diff > 1990);
	}

	// Degenerate derivative (attribute 0 constant / not a position): falls back to the stable 0.5 cutoff
	{
		const float p[3] = { 1.f, 2.f, 3.f };
		CHECK(hashed_threshold(p, 0.f) == 0.5f);
		CHECK(hashed_threshold(p, INFINITY) == 0.5f);
		CHECK(hashed_threshold(p, NAN) == 0.5f);
	}

	// Coverage-preserving mip scale: off at k = 0 and on the base level, grows with the mip, clamped to 1
	CHECK(mipscale_alpha(0.4f, 3.f, 0.f) == 0.4f);
	CHECK(mipscale_alpha(0.4f, 0.f, 0.25f) == 0.4f);
	CHECK(mipscale_alpha(0.4f, -2.f, 0.25f) == 0.4f);
	CHECK(mipscale_alpha(0.4f, 2.f, 0.25f) == 0.4f * 1.5f);
	CHECK(mipscale_alpha(0.9f, 4.f, 0.25f) == 1.f);
	CHECK(mipscale_alpha(2.f, 4.f, 0.25f) == 2.f);
	CHECK(mipscale_alpha(0.f, 8.f, 0.25f) == 0.f);

	// VS codegen: attribute 0 is read only when the vertex program declares it; otherwise a constant (FS -> 0.5)
	CHECK(hashpos_assign(true).find("read_location(0)") != std::string::npos);
	CHECK(hashpos_assign(false).find("read_location") == std::string::npos);
	CHECK(hashpos_assign(false).find("r2_hashpos = vec3(0.)") != std::string::npos);
	{ const float z[3] = { 0.f, 0.f, 0.f }; CHECK(hashed_threshold(z, 0.f) == 0.5f); } // constant position: zero derivative -> 0.5
	// RSX_AA_ATEST_HASHED: band parse, hashed reference on the pre-pass, floor on the EQUAL pass, bounded
	CHECK(parse_atest_band(nullptr) == 0.f);
	CHECK(parse_atest_band("") == 0.f);
	CHECK(parse_atest_band("on") == 0.5f);
	CHECK(parse_atest_band("1") == 0.5f);
	CHECK(parse_atest_band("0.25") == 0.25f);
	CHECK(parse_atest_band("3") == 1.f);
	CHECK(parse_atest_band("-1") == 0.f);
	CHECK(atest_hashed_ref(0.5f, 0.9f, 0, 0.5f) == 0.5f);
	CHECK(atest_hashed_ref(0.5f, 0.5f, 1, 0.5f) == 0.5f);
	CHECK(atest_hashed_ref(0.5f, 1.f, 1, 0.5f) == 0.75f);
	CHECK(atest_hashed_ref(0.5f, 0.f, 1, 0.5f) == 0.25f);
	CHECK(atest_hashed_ref(0.5f, 0.f, 2, 0.5f) == 0.25f);
	{
		// The EQUAL pass's floor never rejects what any pre-pass threshold accepted
		bool ok = true;
		for (int r = 1; r < 255; r += 7)
			for (int i = 0; i <= 64; ++i)
			{
				const float ref = r / 255.f, t = i / 64.f;
				ok = ok && atest_hashed_ref(ref, 0.f, 2, 0.5f) <= atest_hashed_ref(ref, t, 1, 0.5f);
				// alpha 1 always passes GREATER, alpha 0 always fails it
				ok = ok && atest_hashed_ref(ref, t, 1, 1.f) < 1.f && atest_hashed_ref(ref, t, 1, 1.f) > 0.f;
			}
		CHECK(ok);
	}
	CHECK(atest_hashed_ref(0.f, 0.f, 2, 0.5f) == 0.f); // ref 0 (GEQUAL 0 = always) unchanged
	std::printf(g_fail ? "test_r2_a2c_hashed: FAIL\n" : "test_r2_a2c_hashed: PASS\n");
	return g_fail;
}
