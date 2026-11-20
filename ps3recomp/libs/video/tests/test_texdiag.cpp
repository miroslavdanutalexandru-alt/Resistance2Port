// Unit test for the [texcache] diagnostic (third_party/rpcs3/rpcs3/Emu/RSX/port_texdiag.h): re-upload
// classification (same / more detail / less detail at the same address), size and mip buckets, sampler
// counters and the 10 s report line (deltas, nothing printed when idle).
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/port_texdiag.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

using namespace r2texdiag;

static uint64_t c(int i) { return g_c[i].load(); }

static void test_classify()
{
	const uint64_t a = pack(0x30001000, 512, 10);
	CHECK(classify(0, a) == -1);                                  // empty slot
	CHECK(classify(pack(0x30002000, 512, 10), a) == -1);          // another address in the slot
	CHECK(classify(a, a) == re_same);
	CHECK(classify(pack(0x30001000, 256, 9), a) == re_bigger);    // was a smaller base level
	CHECK(classify(pack(0x30001000, 512, 1), a) == re_bigger);    // same size, mips added
	CHECK(classify(pack(0x30001000, 1024, 11), a) == re_smaller); // was larger: reduced copy
	CHECK(classify(pack(0x30001000, 512, 11), a) == re_smaller);  // fewer mips
	CHECK(classify(pack(0x30001000, 0xffff, 255), pack(0x30001000, 0xffff, 255)) == re_same); // field limits
}

static void test_buckets()
{
	CHECK(dim_bucket(1) == dim_64 && dim_bucket(64) == dim_64 && dim_bucket(65) == dim_256 && dim_bucket(1024) == dim_1024 && dim_bucket(1025) == dim_big);
	CHECK(mip_bucket(0) == mip_1 && mip_bucket(1) == mip_1 && mip_bucket(2) == mip_4 && mip_bucket(8) == mip_8 && mip_bucket(9) == mip_many);
}

static void test_counters_and_report()
{
	uint64_t last[n_counters]{};
	char line[1024];
	report(last, line, sizeof(line)); // baseline
	CHECK(!report(last, line, sizeof(line)));

	on_upload(0x30010000, 512, 256, 10, true);
	on_upload(0x30010000, 512, 256, 10, true);  // same
	on_upload(0x30010000, 128, 128, 8, true);   // less detail
	on_upload(0x30010000, 512, 512, 10, false); // more detail
	CHECK(c(up_total) == 4 && c(up_shader_read) == 3 && c(up_other) == 1);
	CHECK(c(re_same) == 1 && c(re_smaller) == 1 && c(re_bigger) == 1);
	CHECK(c(dim_512) == 3 && c(dim_256) == 1 && c(mip_many) == 3 && c(mip_8) == 1);

	on_sampler(10.f, 10.f, 0.f, 0.f);
	on_sampler(10.f, 10.f, 2.f, 0.5f); // min_lod clamp + positive bias
	on_sampler(10.f, 1.f, 0.f, 0.f);   // mips requested, one level sampled
	CHECK(c(smp_derived) == 3 && c(smp_mipped) == 2 && c(smp_minlod) == 1 && c(smp_posbias) == 1 && c(smp_mips_dropped) == 1);

	on_fault(true);
	on_fault(false);
	on_fault(false, true); // unclaimed, page already open again: a race
	CHECK(c(av_faults) == 3 && c(av_handled) == 1 && c(av_raced) == 1);

	on_search(false);
	on_search(true);
	on_search(true);
	CHECK(c(search_calls) == 3 && c(search_memo_hits) == 2);

	CHECK(report(last, line, sizeof(line)));
	CHECK(std::strstr(line, "uploads 4 (shader-read 3, other 1;") != nullptr);
	CHECK(std::strstr(line, "re-uploads same 1 more-detail 1 LESS-detail 1") != nullptr);
	CHECK(std::strstr(line, "faults 3 (handled 1, raced 1)") != nullptr);
	CHECK(std::strstr(line, "lookups 3 (search memo 2)") != nullptr);
	CHECK(!report(last, line, sizeof(line))); // deltas: idle again
	on_upload(0x30020000, 64, 64, 1, true);
	CHECK(report(last, line, sizeof(line)) && std::strstr(line, "uploads 1 (") != nullptr);
}

int main()
{
	test_classify();
	test_buckets();
	test_counters_and_report();
	std::printf(g_fail ? "test_texdiag: FAILED\n" : "test_texdiag: all passed\n");
	return g_fail;
}
