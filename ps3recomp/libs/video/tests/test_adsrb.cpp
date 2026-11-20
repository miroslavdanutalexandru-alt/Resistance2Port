// Unit test for the asynchronous surface readback logic (third_party/rpcs3/rpcs3/Emu/RSX/port_adsrb.h):
// RSX_ADS_ASYNC_RB / RSX_ADS_STALE_OK switches, hot-surface tracking and kick budget, the in-flight (pending) bookkeeping that
// lets blits drop early copies, the stale-data fallback and the label-wait split of the [ads-rb] log line.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/port_adsrb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

// PORT_NOTIFY_ALL_WRITES: the runtime's SPU coherence notify (spu_coherency.c), recorded
static uint32_t g_nr_ea, g_nr_len, g_nr_n;
extern "C" void spu_coh_notify_range(uint32_t ea, uint32_t size) { g_nr_ea = ea; g_nr_len = size; g_nr_n++; }

static void set_env(const char* name, const char* value)
{
	_putenv_s(name, value ? value : "");
}

static void test_config()
{
	set_env("RSX_ADS_ASYNC_RB", nullptr);
	set_env("RSX_ADS_STALE_OK", nullptr);
	auto c = adsrb::read_config();
	CHECK(c.async && !c.stale_ok); // defaults: async on, stale off

	set_env("RSX_ADS_STALE_OK", "1");
	c = adsrb::read_config();
	CHECK(c.async && c.stale_ok);

	set_env("RSX_ADS_ASYNC_RB", "0"); // exact upstream behaviour: the fallback is off too
	c = adsrb::read_config();
	CHECK(!c.async && !c.stale_ok);

	set_env("RSX_ADS_ASYNC_RB", nullptr);
	set_env("RSX_ADS_STALE_OK", nullptr);
}

static const adsrb::range dof{ 0x30030000, 0x30030000 + 8192 * 704 - 1 };

static void test_hot_tracking()
{
	adsrb::hot_tracker t;
	adsrb::range out[adsrb::hot_tracker::max_entries];
	const uint64_t ms = 1'000'000;
	uint64_t now = 1000 * ms;

	CHECK(!t.maybe_hot(now));
	t.on_flush(dof, now);
	CHECK(t.maybe_hot(now));
	CHECK(!t.is_hot(dof, now));                                 // one flush is not a pattern
	CHECK(t.collect(now, 0, 0xFFFFFFFFu, out) == 0);

	now += 33 * ms;
	t.on_flush(dof, now);                                        // next frame: hot
	CHECK(t.is_hot(dof, now));
	CHECK(t.collect(now, 0, 0xFFFFFFFFu, out) == 1 && out[0].start == dof.start && out[0].end == dof.end);
	CHECK(t.collect(now, dof.end, dof.end + 100, out) == 1);     // overlap on the last byte
	CHECK(t.collect(now, dof.end + 1, dof.end + 100, out) == 0); // no overlap
	CHECK(t.collect(now, 0, dof.start - 1, out) == 0);

	// kick budget per flush cycle, refilled by the next flush
	for (uint32_t i = 0; i < adsrb::hot_tracker::max_kicks; ++i)
	{
		CHECK(t.collect(now, 0, 0xFFFFFFFFu, out) == 1);
		t.on_kick(dof);
	}
	CHECK(t.collect(now, 0, 0xFFFFFFFFu, out) == 0);
	CHECK(t.any_pending());
	CHECK(t.collect(0, 0, 0xFFFFFFFFu, out, adsrb::hot_tracker::select::pending) == 1);
	now += 33 * ms;
	t.on_flush(dof, now);
	CHECK(!t.any_pending());                                     // the flush consumed the copy
	CHECK(t.collect(now, 0, 0xFFFFFFFFu, out) == 1);

	// cools down when no longer read back
	CHECK(!t.is_hot(dof, now + adsrb::hot_tracker::hot_window_ns + 1));
	CHECK(!t.maybe_hot(now + adsrb::hot_tracker::hot_window_ns + 1));
	CHECK(t.collect(now + adsrb::hot_tracker::hot_window_ns + 1, 0, 0xFFFFFFFFu, out) == 0);
	now += adsrb::hot_tracker::hot_window_ns + 1;
	t.on_flush(dof, now);                                        // streak restarts
	CHECK(!t.is_hot(dof, now));
}

static void test_pending_never_evicted()
{
	adsrb::hot_tracker t;
	adsrb::range out[adsrb::hot_tracker::max_entries];
	const uint64_t ms = 1'000'000;
	uint64_t now = 5000 * ms;

	adsrb::range r[adsrb::hot_tracker::max_entries];
	for (int i = 0; i < adsrb::hot_tracker::max_entries; ++i)
	{
		r[i] = { 0x1000000u * (i + 1), 0x1000000u * (i + 1) + 0xFFFF };
		t.on_flush(r[i], now);
		t.on_flush(r[i], now + ms);
		t.on_kick(r[i]);                                         // copy in flight
	}
	now += 2 * ms;

	// a new range cannot push out a range whose early copy is still in flight
	const adsrb::range other{ 0x40000000, 0x4000FFFF };
	t.on_flush(other, now);
	t.on_flush(other, now + ms);
	CHECK(!t.is_hot(other, now + ms));
	CHECK(t.collect(0, 0, 0xFFFFFFFFu, out, adsrb::hot_tracker::select::pending) == adsrb::hot_tracker::max_entries);

	// once one of them is flushed it can be replaced, and only that one
	t.on_flush(r[2], now + 2 * ms);
	t.on_flush(other, now + 3 * ms);
	t.on_flush(other, now + 4 * ms);
	CHECK(t.is_hot(other, now + 4 * ms));
	CHECK(t.collect(0, 0, 0xFFFFFFFFu, out, adsrb::hot_tracker::select::pending) == adsrb::hot_tracker::max_entries - 1);
	CHECK(t.collect(0, r[2].start, r[2].end, out, adsrb::hot_tracker::select::pending) == 0);
	CHECK(t.collect(0, r[0].start, r[0].end, out, adsrb::hot_tracker::select::pending) == 1);
}

static void test_stale_store()
{
	adsrb::stale_store s;
	std::string frame1(4096, 'a'), frame2(4096, 'b');
	std::string guest(4096, 'x');

	CHECK(!s.try_serve(dof.start, 4096, reinterpret_cast<uint8_t*>(guest.data()))); // nothing to serve yet
	CHECK(guest == std::string(4096, 'x'));

	s.refresh(dof.start, 4096, reinterpret_cast<const uint8_t*>(frame1.data()), 1);
	CHECK(!s.try_serve(dof.start, 2048, reinterpret_cast<uint8_t*>(guest.data())));  // other length
	CHECK(!s.try_serve(dof.start + 4, 4096, reinterpret_cast<uint8_t*>(guest.data()))); // other range
	CHECK(s.try_serve(dof.start, 4096, reinterpret_cast<uint8_t*>(guest.data())));
	CHECK(guest == frame1);                                     // exactly the previous real readback
	CHECK(g_nr_n == 1 && g_nr_ea == dof.start && g_nr_len == 4096); // the serve is a guest write: SPU reservations notified
	CHECK(!s.try_serve(dof.start, 4096, reinterpret_cast<uint8_t*>(guest.data())));  // never twice in a row

	s.refresh(dof.start, 4096, reinterpret_cast<const uint8_t*>(frame2.data()), 2);
	CHECK(s.try_serve(dof.start, 4096, reinterpret_cast<uint8_t*>(guest.data())));
	CHECK(guest == frame2);
}

static void test_label_split_and_report()
{
	const uint64_t ms = 1'000'000;
	adsrb::label_split ls{};

	// wait 0..20 ms, SPU fault readback 5..6 ms: 5 before + 1 readback + 14 after
	adsrb::stats::split_episode(ls, 100 * ms, 120 * ms, 105 * ms, 106 * ms);
	CHECK(ls.n == 1 && ls.with_fault == 1);
	CHECK(ls.pre_ns == 5 * ms && ls.flush_ns == 1 * ms && ls.post_ns == 14 * ms && ls.total_ns == 20 * ms);

	// fault started before the wait: only the part inside counts as readback
	adsrb::stats::split_episode(ls, 200 * ms, 210 * ms, 198 * ms, 202 * ms);
	CHECK(ls.with_fault == 2 && ls.pre_ns == 5 * ms && ls.flush_ns == 3 * ms && ls.post_ns == 22 * ms);

	// fault finished before the wait began
	adsrb::stats::split_episode(ls, 300 * ms, 305 * ms, 290 * ms, 291 * ms);
	CHECK(ls.n == 3 && ls.with_fault == 2 && ls.fault_before == 1);

	// short waits are not label stalls
	adsrb::stats::split_episode(ls, 400 * ms, 400 * ms + 500'000, 0, 0);
	CHECK(ls.n == 3);

	// polls 100 us apart form one episode; a gap starts the next one
	adsrb::stats st;
	uint64_t t = 1000 * ms;
	CHECK(!st.report(t, nullptr, 0)); // arms the period
	for (int i = 0; i <= 100; ++i) st.on_label_poll(t + i * 100'000);     // 10 ms wait
	st.on_av(t + 2 * ms, t + 3 * ms);
	st.on_flush_wait(true, true, true, 1000);
	st.on_flush_wait(true, true, false, 300'000);
	st.on_flush_wait(false, false, false, 900'000);
	st.kicks.fetch_add(2);
	st.on_label_poll(t + 50 * ms);                                       // closes the first episode
	char buf[640];
	CHECK(!st.report(t + 1 * ms, buf, sizeof(buf)));                     // period not over
	CHECK(st.report(t + adsrb::stats::report_ns + 60 * ms, buf, sizeof(buf)));
	const std::string line(buf);
	std::printf("%s\n", buf);
	CHECK(line.find("[ads-rb]") == 0);
	CHECK(line.find("3 flushes (hot 2)") != std::string::npos);
	CHECK(line.find("max 900 us") != std::string::npos);
	CHECK(line.find("async hits 1 late 1 misses 1") != std::string::npos);
	CHECK(line.find("kicks 2") != std::string::npos);
	CHECK(line.find("with readback 1: before-fault 2.00 + readback 1.00 + after 7.00 ms") != std::string::npos);
	CHECK(st.flushes.load() == 0 && st.kicks.load() == 0);               // counters restart
}

// R2PORT RSX_ADS_STALE_AUTO: a surface is chronic after 4 late flushes in a row; one in-time flush clears it
static void test_late_tracker()
{
	adsrb::late_tracker lt;
	for (int i = 0; i < 3; ++i) lt.on_flush(0x1000, true);
	CHECK(!lt.chronic(0x1000) && lt.streak(0x1000) == 3);
	lt.on_flush(0x1000, true);
	CHECK(lt.chronic(0x1000));
	CHECK(!lt.chronic(0x2000));
	lt.on_flush(0x1000, false);
	CHECK(!lt.chronic(0x1000) && lt.streak(0x1000) == 0);
	lt.on_flush(0x3000, false);                       // in time, never late: not tracked
	CHECK(lt.streak(0x3000) == 0);
	for (uint32_t s = 1; s <= 5; ++s) for (uint32_t k = 0; k < s; ++k) lt.on_flush(s << 16, true);
	CHECK(lt.streak(5u << 16) == 5 && lt.streak(4u << 16) == 4); // the lowest streak was evicted
	CHECK(lt.streak(1u << 16) == 0);
}

// R2PORT RSX_ADS_KICK_ADAPT (from R3): a range whose early copies are always replaced by the fault recopy stops kicking after
// adapt_idle unconsumed cycles, probes on every adapt_probe-th flush, and resumes once a probe copy is consumed. Off = unchanged.
static void test_kick_adapt()
{
	set_env("RSX_ADS_KICK_ADAPT", nullptr);
	CHECK(adsrb::read_config().kick_adapt);
	set_env("RSX_ADS_KICK_ADAPT", "0");
	CHECK(!adsrb::read_config().kick_adapt);
	set_env("RSX_ADS_ASYNC_RB", "0");
	set_env("RSX_ADS_KICK_ADAPT", nullptr);
	CHECK(!adsrb::read_config().kick_adapt);
	set_env("RSX_ADS_ASYNC_RB", nullptr);

	const uint64_t ms = 1'000'000;
	for (int adapt = 0; adapt < 2; ++adapt)
	{
		adsrb::hot_tracker t;
		t.set_adapt(adapt != 0);
		adsrb::range out[adsrb::hot_tracker::max_entries];
		uint64_t now = 3000 * ms;
		t.on_flush(dof, now);
		int kicks = 0, suppressed_cycles = 0;
		for (int f = 0; f < 200; ++f)
		{
			now += 16 * ms;
			uint32_t sup = 0;
			if (t.collect(now, 0, 0xFFFFFFFFu, out, adsrb::hot_tracker::select::kick, &sup) == 1)
			{
				t.on_kick(dof);
				kicks++;
			}
			if (sup) suppressed_cycles++;
			t.on_flush(dof, now, false);
		}
		if (!adapt)
		{
			CHECK(kicks == 199 && suppressed_cycles == 0);
		}
		else
		{
			CHECK(kicks >= 3 && kicks <= 3 + 200 / int(adsrb::hot_tracker::adapt_probe) + 1);
			CHECK(suppressed_cycles == 199 - kicks);
			uint32_t sup = 0;
			while (t.collect(now, 0, 0xFFFFFFFFu, out, adsrb::hot_tracker::select::kick, &sup) == 0)
			{
				now += 16 * ms;
				t.on_flush(dof, now, false);
			}
			t.on_kick(dof);
			now += 16 * ms;
			t.on_flush(dof, now, true);
			CHECK(t.collect(now, 0, 0xFFFFFFFFu, out) == 1);
			now += 16 * ms;
			t.on_flush(dof, now, false); // no kick this cycle: idle does not grow
			CHECK(t.collect(now, 0, 0xFFFFFFFFu, out) == 1);
		}
	}
}

int main()
{
	test_config();
	test_hot_tracking();
	test_pending_never_evicted();
	test_stale_store();
	test_label_split_and_report();
	test_late_tracker();
	test_kick_adapt();
	std::printf(g_fail ? "test_adsrb: FAIL\n" : "test_adsrb: all passed\n");
	return g_fail;
}
