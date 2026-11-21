// R2PORT: guest/host time helpers normally provided by rpcs3/Emu/Cell/lv2/sys_time.cpp.
// Same units as RPCS3: get_system_time() is microseconds, get_timebased_time() uses the
// 80 MHz PS3 timebase. Both are relative to the first call (no host uptime leaks to the guest).
#include "stdafx.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Cell/lv2/sys_time.h"
#include "Emu/system_config.h"

#include <Windows.h>

static constexpr u64 g_timebase_freq = 80000000ull;

namespace
{
	struct qpc_info
	{
		u64 freq = 0;
		u64 start = 0;

		qpc_info()
		{
			LARGE_INTEGER f, c;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&c);
			freq = f.QuadPart;
			start = c.QuadPart;
		}
	};

	const qpc_info& qpc()
	{
		static const qpc_info s_info;
		return s_info;
	}

	// R2PORT: QPC runs at 10 MHz: the two common outputs (1 MHz, 80 MHz) are an exact divide / multiply.
	// Same value as the general formula below, minus two 64-bit divisions per call.
	u64 qpc_scaled(u64 out_freq)
	{
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		const qpc_info& q = qpc();
		const u64 t = c.QuadPart - q.start;
		const u64 f = q.freq;
		if (f == 10'000'000ull)
		{
			if (out_freq == 1'000'000ull) return t / 10;
			if (out_freq == 80'000'000ull) return t * 8;
		}
		return t / f * out_freq + (t % f) * out_freq / f;
	}
}

u64 get_system_time()
{
	// Never 0 (RPCS3 callers treat 0 as "unset")
	return qpc_scaled(1000000ull) + 1;
}

u64 get_guest_system_time(u64 time)
{
	return (time != umax ? time : get_system_time()) * g_cfg.core.clocks_scale / 100;
}

u64 get_timebased_time()
{
	return (qpc_scaled(g_timebase_freq) + 1) * g_cfg.core.clocks_scale / 100;
}

u64 sys_time_get_timebase_frequency()
{
	return g_timebase_freq;
}
