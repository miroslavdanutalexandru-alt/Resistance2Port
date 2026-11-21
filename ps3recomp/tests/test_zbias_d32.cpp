/* RSX_ZBIAS_D32 (third_party/rpcs3/rpcs3/Emu/RSX/VK/VKR2DepthBias.h): pure logic. */
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "VKR2DepthBias.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
	CHECK(r2zb::parse_mode(nullptr) == -1);
	CHECK(r2zb::parse_mode("") == -1);
	CHECK(r2zb::parse_mode("auto") == -1);
	CHECK(r2zb::parse_mode("0") == 0);
	CHECK(r2zb::parse_mode("1") == 1);

	/* auto: only on the D32F fallback, only for fixed Z24 surfaces with fill offset enabled */
	CHECK(r2zb::active(-1, false, true, true));
	CHECK(!r2zb::active(-1, true, true, true));
	CHECK(!r2zb::active(-1, false, false, true));
	CHECK(!r2zb::active(-1, false, true, false));
	CHECK(!r2zb::active(0, false, true, true));
	CHECK(r2zb::active(1, true, true, true));
	CHECK(!r2zb::active(1, true, true, false));

	/* points/lines never biased (Vulkan depth bias is polygon-only) */
	CHECK(!r2zb::polygon_primitive(1));
	CHECK(!r2zb::polygon_primitive(2));
	CHECK(!r2zb::polygon_primitive(3));
	CHECK(!r2zb::polygon_primitive(4));
	for (int p = 5; p <= 10; p++) CHECK(r2zb::polygon_primitive(p));
	CHECK(!r2zb::active(-1, false, r2zb::polygon_primitive(2), true));

	/* one RSX unit is one D24 step; 16777215 units span the whole range */
	CHECK(r2zb::window_offset(0.f) == 0.f);
	CHECK(std::fabs(r2zb::window_offset(1.f) * 16777215.0 - 1.0) < 1e-6);
	CHECK(std::fabs(r2zb::window_offset(16777215.f) - 1.f) < 1e-7);
	CHECK(r2zb::window_offset(-4.f) < 0.f);

	/* the reason for the switch: Vulkan's D32F constant unit 2^(e-23) vs the D24 unit at z = 0.1 */
	{
		int e; std::frexp(0.1f, &e); /* 0.1 = m * 2^e, m in [0.5,1) -> exponent e-1 */
		const double vk_unit = std::ldexp(1.0, (e - 1) - 23);
		const double rsx_unit = r2zb::window_offset(1.f);
		CHECK(vk_unit < rsx_unit / 8.0);
	}

	if (!fails) std::printf("zbias_d32: all passed\n");
	return fails ? 1 : 0;
}