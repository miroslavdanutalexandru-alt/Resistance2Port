// Unit test for the round-8 Vulkan renderer upload memos (third_party/rpcs3/rpcs3/Emu/RSX/port_r2memo.h):
// index upload dedup (R2_IDX_DEDUP), transform-constant memo (R2_XFORM_MEMO), exact decode-parameter packing,
// the env switch parsing, the sampler parameter memo (RSX_SAMPLER_CACHE) and the multi-block vertex reuse
// (RSX_VTX_REUSE). Every reuse must be impossible unless the upstream path would write identical bytes.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/port_r2memo.h"

#include <cstdio>
#include <cstdlib>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

using r2memo::index_dedup;

static void test_index_dedup()
{
	static index_dedup t; // large: keep off the stack
	unsigned short src[64];
	for (int i = 0; i < 64; ++i) src[i] = static_cast<unsigned short>(i * 3);
	const uint64_t p = r2memo::index_params(1, 5, false, false, 0);
	const r2memo::index_result r{ 0x1200, 3, 189, 64 };
	r2memo::index_result out{};

	CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::miss);
	t.store(src, sizeof(src), p, 128, 7, r);
	CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::hit);
	CHECK(out.offset == 0x1200 && out.min_index == 3 && out.max_index == 189 && out.index_count == 64);

	// any key difference is a miss
	CHECK(t.lookup(src, sizeof(src), p, 128, 8, out) == index_dedup::result::miss);                 // heap epoch moved
	CHECK(t.lookup(src, sizeof(src), p, 256, 7, out) == index_dedup::result::miss);                 // different upload size
	CHECK(t.lookup(src, sizeof(src) - 2, p, 128, 7, out) == index_dedup::result::miss);             // different source range
	CHECK(t.lookup(src + 1, sizeof(src) - 2, p, 128, 7, out) == index_dedup::result::miss);         // different source start
	CHECK(t.lookup(src, sizeof(src), r2memo::index_params(1, 6, false, false, 0), 128, 7, out) == index_dedup::result::miss);

	// same key, changed guest bytes: stale (re-upload), then the refreshed entry hits again
	src[17] ^= 1;
	CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::stale);
	t.store(src, sizeof(src), p, 128, 7, { 0x2400, 3, 189, 64 });
	CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::hit && out.offset == 0x2400);

	// size limits: empty and oversized ranges are never cached
	static unsigned char big[index_dedup::max_bytes + 4];
	t.store(big, sizeof(big), p, sizeof(big), 7, r);
	CHECK(t.lookup(big, sizeof(big), p, sizeof(big), 7, out) == index_dedup::result::miss);
	t.store(src, 0, p, 0, 7, r);
	CHECK(t.lookup(src, 0, p, 0, 7, out) == index_dedup::result::miss);
	t.store(big, index_dedup::max_bytes, p, index_dedup::max_bytes, 7, r);
	CHECK(t.lookup(big, index_dedup::max_bytes, p, index_dedup::max_bytes, 7, out) == index_dedup::result::hit);

	// a colliding store evicts: the evicted key must miss, never return the other entry's result
	unsigned short other[64] = {};
	uint32_t n = 0;
	const uint32_t want = index_dedup::slot_of(src, sizeof(src), p);
	const unsigned char* base = reinterpret_cast<const unsigned char*>(big);
	const unsigned char* coll = nullptr;
	for (uint32_t off = 0; off + sizeof(other) <= sizeof(big) && !coll; off += 2, ++n)
		if (base + off != reinterpret_cast<const unsigned char*>(src) && index_dedup::slot_of(base + off, sizeof(other), p) == want) coll = base + off;
	CHECK(coll != nullptr);
	if (coll)
	{
		t.store(coll, sizeof(other), p, 128, 7, { 0x9900, 0, 0, 64 });
		CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::miss);
		CHECK(t.lookup(coll, sizeof(other), p, 128, 7, out) == index_dedup::result::hit && out.offset == 0x9900);
	}
}

static void test_index_params()
{
	std::set<uint64_t> seen;
	for (uint32_t type = 0; type < 2; ++type)
		for (uint32_t prim = 0; prim < 11; ++prim)
			for (int re = 0; re < 2; ++re)
				for (int em = 0; em < 2; ++em)
					for (uint32_t ri : { 0u, 0xFFFFu, 0xFFFFFFFFu })
					{
						const uint64_t k = r2memo::index_params(type, prim, re != 0, em != 0, ri);
						if (re) CHECK(seen.insert(k).second); // every distinct parameter set packs distinctly
						else seen.insert(k);
					}
	// with restart disabled the restart index does not change the upload, so it must not split the key
	CHECK(r2memo::index_params(1, 5, false, false, 0) == r2memo::index_params(1, 5, false, false, 0xFFFF));
	CHECK(r2memo::index_params(1, 5, true, false, 0) != r2memo::index_params(1, 5, true, false, 0xFFFF));
	CHECK(r2memo::index_params(1, 5, true, false, 0xFFFF) != r2memo::index_params(1, 5, true, true, 0xFFFF));
}

static void test_xform_memo()
{
	r2memo::xform_memo m;
	int prog_a = 0, prog_b = 0;
	CHECK(!m.hit(&prog_a, 1, 1));
	CHECK(!m.hit(nullptr, 0, 0)); // empty memo never hits, even for a null key
	m.store(&prog_a, 5, 9);
	CHECK(m.hit(&prog_a, 5, 9));
	CHECK(!m.hit(&prog_b, 5, 9));  // program changed
	CHECK(!m.hit(&prog_a, 6, 9));  // constants written
	CHECK(!m.hit(&prog_a, 5, 10)); // heap epoch / frame moved
	m.clear();
	CHECK(!m.hit(&prog_a, 5, 9));

	const uint64_t g = r2memo::g_xform_gen, e = r2memo::g_heap_epoch;
	r2memo::bump_xform_gen();
	r2memo::bump_heap_epoch();
	CHECK(r2memo::g_xform_gen == g + 1 && r2memo::g_heap_epoch == e + 1);
}

static void test_env_switch()
{
	_putenv("R2MEMO_TEST_SW=");
	CHECK(r2memo::env_switch("R2MEMO_TEST_SW", true) == true);
	CHECK(r2memo::env_switch("R2MEMO_TEST_SW", false) == false);
	_putenv("R2MEMO_TEST_SW=0");
	CHECK(r2memo::env_switch("R2MEMO_TEST_SW", true) == false);
	_putenv("R2MEMO_TEST_SW=1");
	CHECK(r2memo::env_switch("R2MEMO_TEST_SW", false) == true);
}

static void test_pipe_cache_header()
{
	uint8_t uuid[16];
	for (int i = 0; i < 16; ++i) uuid[i] = static_cast<uint8_t>(0xA0 + i);
	std::vector<uint8_t> blob(64, 0x55);
	const uint32_t h[4] = { 32, 1, 0x10DE, 0x2684 };
	std::memcpy(blob.data(), h, sizeof(h));
	std::memcpy(blob.data() + 16, uuid, 16);
	CHECK(r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2684, uuid));
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), 31, 0x10DE, 0x2684, uuid));      // truncated
	CHECK(!r2memo::pipe_cache_header_ok(nullptr, 0, 0x10DE, 0x2684, uuid));
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x1002, 0x2684, uuid)); // other vendor
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2204, uuid)); // other device
	uint8_t uuid2[16];
	std::memcpy(uuid2, uuid, 16);
	uuid2[15] ^= 1;
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2684, uuid2)); // other driver build
	blob[4] = 2;
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2684, uuid)); // header version
	blob[4] = 1;
	blob[0] = 16;
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2684, uuid)); // header size too small
}

// RSX_SAMPLER_CACHE: keyed_memo must return exactly what the memoised pure function returns, for every key, and must
// never answer a key that differs in any single bit of any word.
struct smp_val
{
	float a = 0.f, b = 0.f;
	uint32_t c = 0;
};

static smp_val smp_derive(const uint32_t (&k)[17])
{
	smp_val v;
	uint32_t h = 0x811C9DC5u;
	for (uint32_t w : k) { h ^= w; h *= 16777619u; }
	v.a = static_cast<float>(k[0] & 0xFFF) * 0.25f + static_cast<float>(k[14] & 0xFF);
	v.b = static_cast<float>(h >> 8);
	v.c = h;
	return v;
}

static void test_keyed_memo()
{
	using memo_t = r2memo::keyed_memo<17, smp_val>;
	memo_t m;
	uint32_t k[17];
	for (uint32_t i = 0; i < 17; ++i) k[i] = 0x1000u * i + 7;

	CHECK(m.find(k) == nullptr);
	m.store(k, smp_derive(k));
	const smp_val* hit = m.find(k);
	CHECK(hit && hit->c == smp_derive(k).c);

	// one flipped bit in any word -> miss
	for (uint32_t w = 0; w < 17; ++w)
	{
		for (uint32_t bit : { 0u, 13u, 31u })
		{
			k[w] ^= 1u << bit;
			CHECK(m.find(k) == nullptr);
			k[w] ^= 1u << bit;
		}
	}
	CHECK(m.find(k) != nullptr);

	// RSX_SAMPLER_CACHE_WIDE=0 (single set) round-robin: four distinct keys fit, the fifth evicts the oldest
	uint32_t keys[5][17];
	memo_t r;
	r.set_wide(false);
	for (uint32_t n = 0; n < 5; ++n)
	{
		for (uint32_t i = 0; i < 17; ++i) keys[n][i] = n * 100 + i;
		r.store(keys[n], smp_derive(keys[n]));
	}
	CHECK(r.find(keys[0]) == nullptr);
	for (uint32_t n = 1; n < 5; ++n) CHECK(r.find(keys[n]) && r.find(keys[n])->c == smp_derive(keys[n]).c);
	r.clear();
	for (uint32_t n = 0; n < 5; ++n) CHECK(r.find(keys[n]) == nullptr);

	// wide (default): 64 distinct keys, each set keeps its own 4 ways, so far more than 4 survive; every hit is exact
	memo_t wm;
	wm.set_wide(true);
	uint32_t wkeys[64][17];
	for (uint32_t n = 0; n < 64; ++n)
	{
		for (uint32_t i = 0; i < 17; ++i) wkeys[n][i] = n * 0x9E3779B1u + i * 31u;
		wm.store(wkeys[n], smp_derive(wkeys[n]));
	}
	uint32_t kept = 0;
	for (uint32_t n = 0; n < 64; ++n)
	{
		if (const smp_val* f = wm.find(wkeys[n])) { ++kept; CHECK(f->c == smp_derive(wkeys[n]).c); }
	}
	CHECK(kept > 16);
	wm.clear();
	for (uint32_t n = 0; n < 64; ++n) CHECK(wm.find(wkeys[n]) == nullptr);

	// randomized equivalence, both modes: memo-on path (find, else derive + store) vs memo-off (always derive), keys
	// drawn from a small pool with single-word mutations so hits, misses and near-miss keys all occur
	uint32_t mode_hits[2]{};
	for (int wide = 0; wide < 2; ++wide)
	{
	memo_t e;
	e.set_wide(wide != 0);
	uint32_t pool[24][17];
	uint32_t seed = 12345;
	const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
	for (auto& p : pool) for (uint32_t& w : p) w = rnd();
	uint32_t hits = 0, misses = 0;
	for (int it = 0; it < 200000; ++it)
	{
		uint32_t key[17];
		std::memcpy(key, pool[rnd() % 24], sizeof(key));
		if ((rnd() & 7) == 0) key[rnd() % 17] ^= 1u << (rnd() % 32);
		const smp_val ref = smp_derive(key);
		smp_val got;
		if (const smp_val* f = e.find(key)) { got = *f; ++hits; }
		else { got = smp_derive(key); e.store(key, got); ++misses; }
		if (got.c != ref.c || got.a != ref.a || got.b != ref.b) { CHECK(!"keyed_memo returned a different value"); break; }
	}
	CHECK(hits > 0 && misses > 0);
	mode_hits[wide] = hits;
	}
	CHECK(mode_hits[1] > mode_hits[0] * 2); // 24 live keys: wide holds them, 4 ways thrash
}

// RSX_VTX_REUSE: the bytes a draw sees (ring at the returned offset) must always equal the current concatenation of
// its guest blocks, as the upstream upload would write them.
static void test_vertex_reuse()
{
	using r2memo::vertex_reuse;
	using r2memo::vtx_block;
	static vertex_reuse t;
	unsigned char a[96], b[40];
	for (int i = 0; i < 96; ++i) a[i] = static_cast<unsigned char>(i);
	for (int i = 0; i < 40; ++i) b[i] = static_cast<unsigned char>(200 - i);
	vtx_block bl[2] = { { a, 96 }, { b, 40 } };
	uint64_t off = 0;

	CHECK(t.lookup(bl, 2, 136, 5, off) == vertex_reuse::result::miss);
	t.note(bl, 2, 136, 5);
	CHECK(t.lookup(bl, 2, 136, 5, off) == vertex_reuse::result::seen);
	const unsigned char* snap = t.store(bl, 2, 136, 5, 0x4000);
	CHECK(snap && std::memcmp(snap, a, 96) == 0 && std::memcmp(snap + 96, b, 40) == 0);
	CHECK(t.lookup(bl, 2, 136, 5, off) == vertex_reuse::result::hit && off == 0x4000);

	CHECK(t.lookup(bl, 2, 136, 6, off) == vertex_reuse::result::miss);       // heap epoch moved
	vtx_block swapped[2] = { bl[1], bl[0] };
	CHECK(t.lookup(swapped, 2, 136, 5, off) == vertex_reuse::result::miss);  // block order changes the bytes
	vtx_block shorter[2] = { { a, 92 }, { b, 40 } };
	CHECK(t.lookup(shorter, 2, 132, 5, off) == vertex_reuse::result::miss);
	vtx_block shifted[2] = { { a + 4, 92 }, { b, 40 } };
	CHECK(t.lookup(shifted, 2, 132, 5, off) == vertex_reuse::result::miss);
	CHECK(t.lookup(bl, 1, 96, 5, off) == vertex_reuse::result::miss);        // fewer blocks
	b[39] ^= 0x80;
	CHECK(t.lookup(bl, 2, 136, 5, off) == vertex_reuse::result::stale);      // last byte of the last block changed
	b[39] ^= 0x80;
	a[0] ^= 1;
	CHECK(t.lookup(bl, 2, 136, 5, off) == vertex_reuse::result::stale);      // first byte of the first block changed
	a[0] ^= 1;
	// a hit must be dropped if a ring allocation after the lookup grew/recycled the heap (epoch or heap_changed)
	CHECK(vertex_reuse::hit_still_valid(5, 5, false));
	CHECK(!vertex_reuse::hit_still_valid(5, 6, false) && !vertex_reuse::hit_still_valid(5, 5, true));
	CHECK(!vertex_reuse::eligible(0, 0) && !vertex_reuse::eligible(vertex_reuse::max_blocks + 1, 64) &&
		!vertex_reuse::eligible(2, vertex_reuse::max_bytes + 1));

	// randomized model: guest memory with random writes, a ring that is scribbled over whenever the epoch moves,
	// draws over a pool of block lists. Every draw must read exactly its current guest bytes.
	static vertex_reuse m;
	std::vector<unsigned char> guest(16384), ring(1 << 20, 0);
	uint32_t seed = 99;
	const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
	for (auto& g : guest) g = static_cast<unsigned char>(rnd());
	struct layout { uint32_t n; uint32_t start[3]; uint32_t size[3]; };
	layout pool[16];
	for (auto& l : pool)
	{
		l.n = 1 + rnd() % 3;
		for (uint32_t i = 0; i < l.n; ++i) { l.size[i] = 16 + rnd() % 600; l.start[i] = rnd() % (16384 - l.size[i]); }
	}
	uint64_t epoch = 1;
	size_t ring_pos = 0;
	uint32_t hits = 0, bad = 0, drops = 0, grows_after_alloc = 0;
	for (int it = 0; it < 100000; ++it)
	{
		if ((rnd() % 200) == 0)
		{
			++epoch;
			for (auto& r : ring) r = static_cast<unsigned char>(rnd());
			ring_pos = 0;
		}
		if ((rnd() % 4) == 0) guest[rnd() % guest.size()] ^= static_cast<unsigned char>(1 + rnd() % 255);

		const layout& l = pool[rnd() % 16];
		vtx_block blk[3];
		uint32_t total = 0;
		for (uint32_t i = 0; i < l.n; ++i) { blk[i] = { guest.data() + l.start[i], l.size[i] }; total += l.size[i]; }

		uint64_t use_off = 0;
		const uint64_t lookup_epoch = epoch;
		auto res = m.lookup(blk, l.n, total, epoch, use_off);
		// the volatile allocation that follows the lookup in upload_vertex_data may grow the heap (new buffer, epoch++)
		if (((rnd() >> 16) % 50) == 0) { ++epoch; ring_pos = 0; for (auto& r : ring) r = static_cast<unsigned char>(rnd()); }
		if (res == vertex_reuse::result::hit && !vertex_reuse::hit_still_valid(lookup_epoch, epoch, false))
		{
			res = vertex_reuse::result::stale;
			++drops;
		}
		if (res == vertex_reuse::result::hit) ++hits;
		else
		{
			if (ring_pos + total > ring.size()) { ++epoch; ring_pos = 0; for (auto& r : ring) r = static_cast<unsigned char>(rnd()); }
			use_off = ring_pos;
			ring_pos += (total + 255) & ~255u;
			// the volatile allocation after the persistent one may grow the heap: the new heap reuses offsets from 0,
			// so recording use_off would let a later hit read overwritten bytes
			const uint64_t alloc_epoch = epoch;
			if (((rnd() >> 16) % 40) == 0) { ++epoch; ring_pos = 0; for (auto& r : ring) r = static_cast<unsigned char>(rnd()); ++grows_after_alloc; }
			const unsigned char* snap = nullptr;
			if (vertex_reuse::can_record(alloc_epoch, epoch, false))
			{
				if (res == vertex_reuse::result::miss) m.note(blk, l.n, total, epoch);
				else snap = m.store(blk, l.n, total, epoch, use_off);
			}
			unsigned char* dst = ring.data() + use_off;
			if (snap) std::memcpy(dst, snap, total);
			else for (uint32_t i = 0; i < l.n; ++i) { std::memcpy(dst, blk[i].src, blk[i].size); dst += blk[i].size; }
		}

		const unsigned char* seen = ring.data() + use_off;
		for (uint32_t i = 0; i < l.n; ++i)
		{
			if (std::memcmp(seen, blk[i].src, blk[i].size) != 0) ++bad;
			seen += blk[i].size;
		}
	}
	CHECK(bad == 0);
	CHECK(hits > 1000);
	CHECK(drops > 0);
	CHECK(grows_after_alloc > 0);
	CHECK(!vertex_reuse::can_record(5, 6, false) && !vertex_reuse::can_record(5, 5, true) && vertex_reuse::can_record(5, 5, false));
}

// RSX_ZCULL_IDLE_FLUSH: the R2 hang of 2026-11-13 (relaxed ZCULL, FIFO idle, a polled report never submitted)
static void test_zcull_idle_flush()
{
	using r2memo::zcull_idle_flush_due;
	// the hang state: relaxed sync, FIFO idle, a claimed report the guest polls (critical)
	CHECK(zcull_idle_flush_due(true, true, true, 969000, 1, true));
	CHECK(zcull_idle_flush_due(true, true, true, 200, 3, true));
	// not yet idle long enough, or the FIFO still runs: later commands may flush anyway
	CHECK(!zcull_idle_flush_due(true, true, true, 199, 1, true));
	CHECK(!zcull_idle_flush_due(true, true, false, 969000, 1, true));
	// nothing claimed, or no guest access yet (protected pages fault into a sync instead)
	CHECK(!zcull_idle_flush_due(true, true, true, 969000, 0, true));
	CHECK(!zcull_idle_flush_due(true, true, true, 969000, 1, false));
	// strict sync has its own time-based flush; RSX_ZCULL_IDLE_FLUSH=0
	CHECK(!zcull_idle_flush_due(true, false, true, 969000, 1, true));
	CHECK(!zcull_idle_flush_due(false, true, true, 969000, 1, true));
}

// RSX_HK_DRAW_LIGHT: only a lone backend interrupt (per draw) takes the light path
static void test_housekeeping_level()
{
	using r2memo::housekeeping_level;
	CHECK(housekeeping_level(false, 0, true) == 0);
	CHECK(housekeeping_level(false, 1, true) == 1);
	CHECK(housekeeping_level(false, 1, false) == 2);
	CHECK(housekeeping_level(true, 1, true) == 2);
	CHECK(housekeeping_level(true, 0, true) == 2);
	// pipe flush, display, DMA control, memory config or context free alongside it: full run
	CHECK(housekeeping_level(false, 1 | 8, true) == 2);
	CHECK(housekeeping_level(false, 4, true) == 2);
	CHECK(housekeeping_level(false, 0x10, true) == 2);
}

// RSX_ZCULL_CRIT_FLUSH_US: early submit of a polled report while the FIFO still runs (R2 perf 2026-11-15)
static void test_zcull_crit_flush()
{
	using r2memo::zcull_crit_flush_due;
	// a polled report recorded 1.5 ms ago, no forced submit yet
	CHECK(zcull_crit_flush_due(1000, true, true, 10000, 8500, 0));
	// exactly at the delay
	CHECK(zcull_crit_flush_due(1000, true, true, 10000, 9000, 9000));
	// too young, or the last forced submit was too recent (bounded to one per delay)
	CHECK(!zcull_crit_flush_due(1000, true, true, 10000, 9001, 0));
	CHECK(!zcull_crit_flush_due(1000, true, true, 10000, 5000, 9500));
	// nothing unsubmitted, no guest access yet, strict sync, switch off
	CHECK(!zcull_crit_flush_due(1000, true, true, 10000, 0, 0));
	CHECK(!zcull_crit_flush_due(1000, true, false, 10000, 5000, 0));
	CHECK(!zcull_crit_flush_due(1000, false, true, 10000, 5000, 0));
	CHECK(!zcull_crit_flush_due(0, true, true, 10000, 5000, 0));
	// a timestamp from the future (taken on a 100 us tick ahead of now) is not due
	CHECK(!zcull_crit_flush_due(1000, true, true, 10000, 10100, 0));
}

int main()
{
	test_index_dedup();
	test_index_params();
	test_xform_memo();
	test_env_switch();
	test_pipe_cache_header();
	test_keyed_memo();
	test_vertex_reuse();
	test_zcull_idle_flush();
	test_housekeeping_level();
	test_zcull_crit_flush();
	std::printf("%s\n", g_fail ? "test_r2memo: FAIL" : "test_r2memo: PASS");
	return g_fail;
}
