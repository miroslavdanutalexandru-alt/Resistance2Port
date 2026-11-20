// cpu-replay (CPU_BOTTLENECK_PLAN.md Q5b): RSX command-stream capture and offline timing replay.
// See scratch\cpu_replay.md for the design, hook sites, usage and limitations.
//
// CAPTURE  (opt-in, inert unless set; the only per-frame cost when unset is one cached-bool test in
//           rsx::thread::on_frame_end):
//   PORT_RSX_CAPTURE=<file.rprx | dir\>     enable (dir\ -> dir\rsx_capture.rprx)
//   PORT_RSX_CAPTURE_FRAMES=N               frames to record (default 3, max 120)
//   PORT_RSX_CAPTURE_AT_FRAME=F             auto-trigger at flip F (default: manual)
//   trigger: Ctrl+Shift+F8 (same chord as PORT_SPU_CAPTURE), or create the file "<file>.go".
//   Uses RPCS3's own frame-capture machinery (RSXFIFO.cpp records every method, rsx_capture.cpp the
//   vertex/index/texture/image_in/notify memory each draw reads). Added here: the IO map, the raw
//   tile/zcull/display-buffer state and live RSX-thread CPU samples at every flip.
//
// REPLAY   PORT_RSX_REPLAY=<file.rprx>  (the process becomes a replay host; the game never runs)
//   PORT_RSX_REPLAY_LOOPS=L   loops over the captured frames (default 6; loop 0 = warm-up, excluded)
//   PORT_RSX_REPLAY_MEM=fast|exact
//        fast  (default) all captured memory is written once up front, the FIFO then runs through all
//              frames without stops: continuous RSX-thread CPU measurement. Dynamic buffers rewritten
//              between draws hold their LAST captured contents (CPU path is the same; images may differ).
//        exact upstream semantics: the FIFO stops before every command with a memory/tile/display state
//              change, the RSX drains, the state is written, the FIFO resumes. Correct data, but every
//              stop is an RSX idle gap (reported separately as idle time).
//   PORT_RSX_REPLAY_FIFO_EA=0x...  guest EA for the replay FIFO (default: first free of a candidate list)
//   Called from r2rsx_context_allocate once the renderer is up. Writes <file>.replay.txt / .csv, then
//   terminates the process.
#include "stdafx.h"
#include "Emu/Memory/vm.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Core/RSXContext.h"
#include "Emu/RSX/Core/RSXEngLock.hpp"
#include "Emu/RSX/Capture/rsx_capture.h"
#include "Emu/RSX/Capture/rsx_replay.h"
#include "Emu/RSX/gcm_enums.h"
#include "util/serialization.hpp"
#include "util/serialization_ext.hpp"
#include "Emu/RSX/Utils/rsx_serde.hpp"
#include "util/asm.hpp"

#include "Emu/RSX/NV47/HW/context_accessors.define.h"

#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" uint8_t* vm_base;
extern "C" uint8_t* vm_sudo_base;

namespace cpu_replay_rsx
{
	extern bool g_on;
	extern bool g_active;

	namespace
	{
		enum class mode : int { off = 0, capture = 1, replay = 2 };

		mode init_mode()
		{
			const char* c = std::getenv("PORT_RSX_CAPTURE");
			const char* r = std::getenv("PORT_RSX_REPLAY");
			if (r && *r) return mode::replay;
			if (c && *c) return mode::capture;
			return mode::off;
		}

		const mode s_mode = init_mode();

		// Raw device state at a flip (and live timing sample of the RSX thread at that flip).
		struct fstate
		{
			u64 qpc = 0;      // QueryPerformanceCounter
			u64 tsc = 0;      // __rdtsc (TSC Hz estimate for the cycle counts)
			u64 cyc = 0;      // QueryThreadCycleTime(RSX thread)
			u64 idle_us = 0;  // rsx::thread::performance_counters.idle_time
			u32 ndisp = 0;
			u32 curdisp = 0;
			RsxDisplayInfo disp[8]{};
			GcmTileInfo tiles[rsx::limits::tiles_count]{};
			GcmZcullInfo zculls[rsx::limits::zculls_count]{};
		};

		struct rprx_hdr
		{
			char magic[4];     // "RPRX"
			u32 version;       // 1
			u32 frames;        // captured flips
			u32 nstates;       // frames + 1 (state[0] = at trigger)
			u32 fstate_size;   // sizeof(fstate) (layout check)
			u32 iomap_n;       // 4096
			u32 tiles_n, zculls_n;
			u32 ncmds, nmem, nmemdata, pad;
			u64 qpc_hz;
			u64 blob_size;     // utils::serial image of rsx::frame_capture_data
		};

		std::string s_path;

		std::string resolve_capture_path(const char* e)
		{
			std::string p = e;
			const DWORD a = GetFileAttributesA(p.c_str());
			if ((!p.empty() && (p.back() == '\\' || p.back() == '/')) || (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)))
			{
				CreateDirectoryA(p.c_str(), nullptr);
				if (p.back() != '\\' && p.back() != '/') p += '\\';
				p += "rsx_capture.rprx";
			}
			return p;
		}

		u64 qpc_now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return static_cast<u64>(q.QuadPart); }
		u64 qpc_hz() { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return static_cast<u64>(q.QuadPart); }

		u64 thread_cycles(HANDLE h)
		{
			ULONG64 c = 0;
			if (h) QueryThreadCycleTime(h, &c);
			return c;
		}

		void beep_async(int n)
		{
			std::thread([n] { for (int i = 0; i < n; i++) Beep(i ? 1600 : 1200, 90); }).detach();
		}

		fstate snapshot(rsx::thread* r, HANDLE rsx_thread)
		{
			fstate s{};
			s.qpc = qpc_now();
			s.tsc = __rdtsc();
			s.cyc = thread_cycles(rsx_thread);
			s.idle_us = r->performance_counters.idle_time.load();
			const rsx::context* ctx = r->get_context();
			s.ndisp = ctx->display_buffers_count;
			s.curdisp = ctx->current_display_buffer;
			std::copy_n(ctx->display_buffers, 8, s.disp);
			std::copy_n(r->tiles, rsx::limits::tiles_count, s.tiles);
			std::copy_n(r->zculls, rsx::limits::zculls_count, s.zculls);
			return s;
		}

		// ---------------------------------------------------------------- capture (RSX thread only)
		int s_cap_state = 0; // 0 wait, 1 recording, 2 done
		u32 s_cap_frames = 3;
		s64 s_cap_at = -1;
		u64 s_flip = 0;
		std::string s_go;
		std::vector<fstate> s_states;
		u32 s_iomap[4096];

		void write_capture(rsx::thread* r)
		{
			utils::serial ar;
			ar(frame_capture);

			rprx_hdr h{};
			std::memcpy(h.magic, "RPRX", 4);
			h.version = 1;
			h.frames = static_cast<u32>(s_states.size() - 1);
			h.nstates = static_cast<u32>(s_states.size());
			h.fstate_size = sizeof(fstate);
			h.iomap_n = 4096;
			h.tiles_n = rsx::limits::tiles_count;
			h.zculls_n = rsx::limits::zculls_count;
			h.ncmds = static_cast<u32>(frame_capture.replay_commands.size());
			h.nmem = static_cast<u32>(frame_capture.memory_map.size());
			h.nmemdata = static_cast<u32>(frame_capture.memory_data_map.size());
			h.qpc_hz = qpc_hz();
			h.blob_size = ar.data.size();

			u64 membytes = 0;
			for (const auto& [blk, idx] : frame_capture.memory_data_map) membytes += blk.data.size();

			std::FILE* f = std::fopen(s_path.c_str(), "wb");
			bool ok = false;
			if (f)
			{
				ok = std::fwrite(&h, sizeof(h), 1, f) == 1;
				ok = ok && std::fwrite(s_iomap, sizeof(s_iomap), 1, f) == 1;
				ok = ok && std::fwrite(s_states.data(), sizeof(fstate), s_states.size(), f) == s_states.size();
				ok = ok && std::fwrite(ar.data.data(), 1, ar.data.size(), f) == ar.data.size();
				ok = (std::fclose(f) == 0) && ok;
			}

			std::fprintf(stderr, "[cpu-replay] rsx: capture %s: %s frames=%u cmds=%u mem_blocks=%u mem_data=%u (%.1f MB) file=%.1f MB\n",
				ok ? "written" : "FAILED", s_path.c_str(), h.frames, h.ncmds, h.nmem, h.nmemdata, membytes / 1048576.0,
				(sizeof(h) + sizeof(s_iomap) + s_states.size() * sizeof(fstate) + ar.data.size()) / 1048576.0);
			std::fflush(stderr);

			frame_capture.reset(*REGS(r->get_context()));
			frame_debug.reset();
			s_states.clear();
			s_states.shrink_to_fit();
		}

		void capture_on_flip(rsx::thread* r)
		{
			s_flip++;
			if (s_cap_state == 2) return;

			if (s_cap_state == 0)
			{
				bool go = s_cap_at >= 0 && s_flip >= static_cast<u64>(s_cap_at);
				if ((GetAsyncKeyState(VK_F8) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
					go = true;
				if (!go && (s_flip % 30) == 0 && GetFileAttributesA(s_go.c_str()) != INVALID_FILE_ATTRIBUTES)
				{
					DeleteFileA(s_go.c_str());
					go = true;
				}
				if (!go || r->capture_current_frame) return;

				// Same start sequence as upstream on_frame_end (g_user_asked_for_frame_capture)
				rsx::context* ctx = r->get_context();
				r->request_guest_write_back();
				r->capture_current_frame = true;
				frame_debug.reset();
				frame_capture.reset(*REGS(ctx));
				frame_capture.replay_commands.reserve(0x40000);

				rsx::frame_capture_data::replay_command replay_cmd;
				replay_cmd.rsx_command = std::make_pair(NV4097_NO_OPERATION, 0);
				frame_capture.replay_commands.push_back(std::move(replay_cmd));
				rsx::capture::capture_display_tile_state(r, frame_capture.replay_commands.back());

				for (u32 i = 0; i < 4096; i++) s_iomap[i] = r->iomap_table.ea[i];

				s_states.clear();
				s_states.reserve(s_cap_frames + 1);
				s_states.push_back(snapshot(r, GetCurrentThread()));
				g_active = true;
				s_cap_state = 1;
				std::fprintf(stderr, "[cpu-replay] rsx: capture started at flip %llu (%u frames)\n", static_cast<unsigned long long>(s_flip), s_cap_frames);
				beep_async(1);
				return;
			}

			s_states.push_back(snapshot(r, GetCurrentThread()));
			if (s_states.size() > s_cap_frames)
			{
				r->capture_current_frame = false;
				write_capture(r);
				g_active = false;
				s_cap_state = 2;
				beep_async(2);
			}
		}

		// ---------------------------------------------------------------- replay
		struct sample { u64 qpc, cyc, idle_us; };

		std::vector<fstate> s_rstates;
		std::vector<std::vector<sample>> s_samples; // [loop][0 = loop start, k = flip k]
		std::atomic<int> s_rloop{ -1 };
		std::atomic<u32> s_rflip{ 0 };

		void apply_fstate(rsx::thread* r, rsx::context* ctx, const fstate& s)
		{
			std::copy_n(s.disp, 8, ctx->display_buffers);
			ctx->display_buffers_count = s.ndisp;
			ctx->current_display_buffer = s.curdisp;
			for (u32 i = 0; i < rsx::limits::tiles_count; i++)
			{
				if (r->tiles[i].bound && !s.tiles[i].bound) r->notify_tile_unbound(i);
				r->tiles[i] = s.tiles[i];
			}
			std::copy_n(s.zculls, rsx::limits::zculls_count, r->zculls);
		}

		void replay_on_flip(rsx::thread* r)
		{
			const int loop = s_rloop.load(std::memory_order_acquire);
			if (loop < 0) return;
			const u32 f = s_rflip.fetch_add(1) + 1;
			auto& v = s_samples[loop];
			if (f < v.size())
				v[f] = { qpc_now(), thread_cycles(GetCurrentThread()), r->performance_counters.idle_time.load() };
			// Device state for the next captured frame (tiles/zcull/display buffers: RSX-thread-owned)
			if (f < s_rstates.size()) apply_fstate(r, r->get_context(), s_rstates[f]);
		}

		void commit(u32 ea, u32 size)
		{
			const u32 a = ea & ~0xFFFu;
			const u32 e = (ea + size + 0xFFFu) & ~0xFFFu;
			for (u8* base : { vm_base, vm_sudo_base })
			{
				if (base && e > a) VirtualAlloc(base + a, e - a, MEM_COMMIT, PAGE_READWRITE);
			}
		}

		HANDLE find_rsx_thread()
		{
			HANDLE found = nullptr;
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) return nullptr;
			THREADENTRY32 te{};
			te.dwSize = sizeof(te);
			const DWORD pid = GetCurrentProcessId();
			for (BOOL ok = Thread32First(snap, &te); ok && !found; ok = Thread32Next(snap, &te))
			{
				if (te.th32OwnerProcessID != pid) continue;
				HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
				if (!h) continue;
				PWSTR d = nullptr;
				if (SUCCEEDED(GetThreadDescription(h, &d)) && d && wcsstr(d, L"rsx::thread"))
				{
					found = h;
					h = nullptr;
				}
				if (d) LocalFree(d);
				if (h) CloseHandle(h);
			}
			CloseHandle(snap);
			return found;
		}

		void set_dmactl(rsx::thread* r, rsx::context* ctx, u32 get, u32 put)
		{
			// Same sequence as sys_rsx.cpp set_rsx_dmactl (lv2 0x001); handled by the RSX thread's
			// dma_control_interrupt (RSXThread.cpp, eng_interrupt_mask).
			const u64 get_put = (u64{put} << 32) | get;
			{
				rsx::eng_lock rlock(r);
				if (r->get_context() == ctx) r->fifo_ctrl->abort();
				while (!ctx->fw.new_get_put.compare_and_swap_test(u64{umax}, get_put)) utils::pause();
				r->eng_interrupt_mask |= rsx::dma_control_interrupt;
			}
			while (ctx->fw.new_get_put != u64{umax}) Sleep(0);
		}

		bool wait_idle(rsx::thread* r, rsx::context* ctx, u32 timeout_ms, bool spin)
		{
			const u64 dl = GetTickCount64() + timeout_ms;
			while (!r->is_fifo_idle(ctx))
			{
				if (GetTickCount64() > dl) return false;
				if (spin) SwitchToThread(); else Sleep(1);
			}
			return true;
		}

		bool read_all(const char* path, std::vector<u8>& out)
		{
			std::FILE* f = std::fopen(path, "rb");
			if (!f) return false;
			_fseeki64(f, 0, SEEK_END);
			const s64 n = _ftelli64(f);
			_fseeki64(f, 0, SEEK_SET);
			out.resize(static_cast<usz>(n > 0 ? n : 0));
			const bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
			std::fclose(f);
			return ok;
		}

		[[noreturn]] void replay_fail(const char* msg)
		{
			std::fprintf(stderr, "[cpu-replay] rsx: replay FAILED: %s\n", msg);
			std::printf("[cpu-replay] rsx: replay FAILED: %s\n", msg);
			std::fflush(stderr);
			std::fflush(stdout);
			TerminateProcess(GetCurrentProcess(), 3);
			for (;;) Sleep(1000);
		}
	}

	bool g_on = s_mode != mode::off;
	bool g_active = false;

	// Hook: top of rsx::thread::on_frame_end (RSX thread), only reached when g_on.
	void on_frame_end(rsx::thread* r)
	{
		if (s_mode == mode::replay)
		{
			replay_on_flip(r);
			return;
		}

		if (s_path.empty())
		{
			s_path = resolve_capture_path(std::getenv("PORT_RSX_CAPTURE"));
			s_go = s_path + ".go";
			if (const char* e = std::getenv("PORT_RSX_CAPTURE_FRAMES")) s_cap_frames = std::clamp<u32>(static_cast<u32>(std::atoi(e)), 1, 120);
			if (const char* e = std::getenv("PORT_RSX_CAPTURE_AT_FRAME")) s_cap_at = std::atoll(e);
			std::fprintf(stderr, "[cpu-replay] rsx: armed -> %s (%u frames; Ctrl+Shift+F8 or create %s)\n", s_path.c_str(), s_cap_frames, s_go.c_str());
		}
		capture_on_flip(r);
	}
}

// Hook: end of r2rsx_context_allocate (libs/video/rpcs3_bridge/r2rsx_bridge.cpp). Returns at once unless
// PORT_RSX_REPLAY is set; otherwise replays and terminates the process.
extern "C" void cpu_replay_rsx_replay(uint32_t /*dma_control_ea*/)
{
	using namespace cpu_replay_rsx;
	const char* path = std::getenv("PORT_RSX_REPLAY");
	if (!path || !*path) return;

	std::vector<u8> file;
	if (!read_all(path, file)) replay_fail("cannot read PORT_RSX_REPLAY file");
	if (file.size() < sizeof(rprx_hdr)) replay_fail("file too small");

	rprx_hdr h{};
	std::memcpy(&h, file.data(), sizeof(h));
	if (std::memcmp(h.magic, "RPRX", 4) || h.version != 1) replay_fail("not an RPRX v1 file");
	if (h.fstate_size != sizeof(fstate) || h.iomap_n != 4096 || h.tiles_n != rsx::limits::tiles_count || h.zculls_n != rsx::limits::zculls_count)
		replay_fail("layout mismatch (captured by a different build of the other game?)");

	usz off = sizeof(h);
	u32 iomap[4096];
	std::memcpy(iomap, file.data() + off, sizeof(iomap));
	off += sizeof(iomap);
	s_rstates.resize(h.nstates);
	std::memcpy(static_cast<void*>(s_rstates.data()), file.data() + off, usz{h.nstates} * sizeof(fstate));
	off += usz{h.nstates} * sizeof(fstate);
	if (off + h.blob_size > file.size()) replay_fail("truncated file");

	auto frame = std::make_unique<rsx::frame_capture_data>();
	{
		utils::serial ar;
		ar.set_reading_state(std::vector<u8>(file.begin() + off, file.begin() + off + h.blob_size));
		ar(*frame);
	}
	file.clear();
	file.shrink_to_fit();
	if (frame->magic != rsx::c_fc_magic) replay_fail("frame_capture_data magic mismatch");

	int loops = 6;
	if (const char* e = std::getenv("PORT_RSX_REPLAY_LOOPS")) loops = std::clamp(std::atoi(e), 1, 1000);
	bool exact = false;
	if (const char* e = std::getenv("PORT_RSX_REPLAY_MEM")) exact = !_stricmp(e, "exact");

	rsx::thread* r = rsx::get_current_renderer();
	rsx::context* ctx = r ? r->get_context() : nullptr;
	if (!ctx) replay_fail("renderer not up");

	// IO map exactly as captured (both directions)
	{
		std::lock_guard lock(r->sys_rsx_mtx);
		for (u32 i = 0; i < 4096; i++)
		{
			r->iomap_table.ea[i].release(~0u);
			r->iomap_table.io[i].release(~0u);
		}
		for (u32 i = 0; i < 4096; i++)
		{
			if (iomap[i] == umax) continue;
			r->iomap_table.ea[i].release(iomap[i]);
			r->iomap_table.io[iomap[i] >> 20].release(i << 20);
		}
	}

	// Memory block index (upstream does a linear find_if per state)
	std::unordered_map<u64, const rsx::frame_capture_data::memory_block*> blk_by_idx;
	std::unordered_map<u64, const rsx::frame_capture_data::memory_block_data*> dat_by_idx;
	for (const auto& [blk, idx] : frame->memory_map) blk_by_idx[idx] = &blk;
	for (const auto& [dat, idx] : frame->memory_data_map) dat_by_idx[idx] = &dat;

	auto translate = [&](u32 offset, u32 location) -> u32
	{
		if (location == 0 /*CELL_GCM_LOCATION_LOCAL*/ || location == 0xFEED0000 /*DMA_MEMORY_FRAME_BUFFER*/)
			return offset < r->local_mem_size ? rsx::constants::local_mem_base + offset : umax;
		if (location == 1 /*CELL_GCM_LOCATION_MAIN*/ || location == 0xFEED0001 /*DMA_MEMORY_HOST_BUFFER*/)
			return r->iomap_table.get_addr(offset);
		return umax;
	};

	u64 n_apply = 0, n_miss = 0, apply_bytes = 0;
	auto apply_state = [&](u64 state)
	{
		const auto b = blk_by_idx.find(state);
		if (b == blk_by_idx.end()) { n_miss++; return; }
		const auto d = dat_by_idx.find(b->second->data_state);
		if (d == dat_by_idx.end()) { n_miss++; return; }
		const u32 ea = translate(b->second->offset, b->second->location);
		const auto& bytes = d->second->data;
		if (ea == umax || bytes.empty()) { n_miss++; return; }
		commit(ea, static_cast<u32>(bytes.size()));
		std::memcpy(vm::base(ea), bytes.data(), bytes.size());
		n_apply++;
		apply_bytes += bytes.size();
	};

	// FIFO buffer: as rsx_replay_thread::alloc_write_fifo, in a free IO range below main_mem_size
	const auto& cmds = frame->replay_commands;
	u64 words = 4;
	{
		u32 cnt = 0;
		for (const auto& rc : cmds)
		{
			if (cnt == 0) { cnt = (rc.rsx_command.first >> 18) & 0x7ff; words++; }
			if (cnt != 0) { words++; cnt--; }
		}
	}
	const u32 fifo_pages = static_cast<u32>((words * 4 + 0xFFFFF) >> 20);
	const u32 io_limit = std::min<u32>(ctx->fw.main_mem_size ? (ctx->fw.main_mem_size >> 20) : 256, 4096);
	u32 io_page = umax;
	for (u32 p = io_limit; p >= fifo_pages && io_page == umax; p--)
	{
		bool free_run = true;
		for (u32 k = p - fifo_pages; k < p && free_run; k++) free_run = iomap[k] == umax;
		if (free_run) io_page = p - fifo_pages;
	}
	if (io_page == umax) replay_fail("no free IO range for the replay FIFO");

	// Pick a FIFO EA that collides with no captured block and no IO-mapped page
	std::vector<std::pair<u32, u32>> used;
	for (const auto& [blk, idx] : frame->memory_map)
	{
		const u32 ea = translate(blk.offset, blk.location);
		const auto d = dat_by_idx.find(blk.data_state);
		if (ea != umax && d != dat_by_idx.end()) used.emplace_back(ea, ea + static_cast<u32>(d->second->data.size()));
	}
	for (u32 i = 0; i < 4096; i++)
		if (iomap[i] != umax) used.emplace_back(iomap[i], iomap[i] + 0x100000);
	std::vector<u32> cands;
	if (const char* e = std::getenv("PORT_RSX_REPLAY_FIFO_EA")) cands.push_back(static_cast<u32>(std::strtoul(e, nullptr, 0)) & ~0xFFFFFu);
	for (u32 c : { 0x48000000u, 0x58000000u, 0x68000000u, 0x78000000u, 0x88000000u, 0x98000000u, 0x28000000u })
		cands.push_back(c);
	u32 fifo_ea = umax;
	for (u32 c : cands)
	{
		const u64 ce = u64{c} + (u64{fifo_pages} << 20);
		if (ce > rsx::constants::local_mem_base) continue;
		const bool clash = std::any_of(used.begin(), used.end(), [&](const auto& u) { return u.first < ce && c < u.second; });
		if (!clash) { fifo_ea = c; break; }
	}
	if (fifo_ea == umax) replay_fail("no free EA for the replay FIFO (set PORT_RSX_REPLAY_FIFO_EA)");
	commit(fifo_ea, fifo_pages << 20);
	{
		std::lock_guard lock(r->sys_rsx_mtx);
		for (u32 k = 0; k < fifo_pages; k++)
		{
			r->iomap_table.ea[io_page + k].release(fifo_ea + (k << 20));
			r->iomap_table.io[(fifo_ea >> 20) + k].release((io_page + k) << 20);
		}
	}

	const u32 fifo_io = io_page << 20;
	std::vector<u32> stops;      // exact mode: IO offset where each state-carrying command starts, then end
	std::vector<usz> stop_cmd;   // replay_commands index of each stop
	u64 n_state_mid = 0;
	{
		auto* w = vm::_ptr<u32>(fifo_ea);
		u32 cur = fifo_io, cnt = 0;
		for (usz i = 0; i < cmds.size(); i++)
		{
			const auto& rc = cmds[i];
			const bool has_state = !rc.memory_state.empty() || rc.display_buffer_state != 0 || rc.tile_state != 0;
			if (has_state)
			{
				if (cnt != 0) n_state_mid++;
				else { stops.push_back(cur); stop_cmd.push_back(i); }
			}
			if (cnt == 0) { cnt = (rc.rsx_command.first >> 18) & 0x7ff; *w++ = rc.rsx_command.first; cur += 4; }
			if (cnt != 0) { *w++ = rc.rsx_command.second; cur += 4; cnt--; }
		}
		stops.push_back(cur);
	}
	const u32 fifo_end = stops.back();

	// Display/tile/zcull device state at capture start
	apply_fstate(r, ctx, s_rstates[0]);

	if (!exact)
	{
		for (const auto& rc : cmds)
			for (u64 st : rc.memory_state) apply_state(st);
	}

	HANDLE rsx_h = find_rsx_thread();
	const u32 frames = h.frames;
	s_samples.assign(loops, std::vector<sample>(frames + 64, sample{}));

	std::printf("[cpu-replay] rsx: replaying %s: frames=%u cmds=%zu fifo=%.1f MB io=0x%08X ea=0x%08X mode=%s loops=%d stops=%zu rsx_thread=%s\n",
		path, frames, cmds.size(), (fifo_end - fifo_io) / 1048576.0, fifo_io, fifo_ea, exact ? "exact" : "fast", loops,
		stops.size() - 1, rsx_h ? "found" : "NOT FOUND (cpu column = 0)");
	std::fflush(stdout);

	const u64 tsc0 = __rdtsc(), q0 = qpc_now();
	std::vector<u32> flips(loops, 0);
	std::vector<sample> loop_end(loops, sample{});
	u32 timeouts = 0;

	for (int loop = 0; loop < loops; loop++)
	{
		if (!wait_idle(r, ctx, 20000, false)) timeouts++;

		// Registers while the RSX is idle (as upstream cpu_task)
		*REGS(ctx) = frame->reg_state;
		ctx->draw_state.pending.set(rsx::draw_state_t::group::all);
		apply_fstate(r, ctx, s_rstates[0]);
		atomic_fence_seq_cst();

		s_rflip.store(0);
		s_samples[loop][0] = { qpc_now(), thread_cycles(rsx_h), r->performance_counters.idle_time.load() };
		s_rloop.store(loop, std::memory_order_release);

		RsxDmaControl* const ctrl = ctx->fifo.ctrl ? ctx->fifo.ctrl : r->ctrl;
		if (!exact)
		{
			set_dmactl(r, ctx, fifo_io, fifo_end);
		}
		else
		{
			set_dmactl(r, ctx, fifo_io, stops[0]);
			for (usz s = 0; s + 1 < stops.size(); s++)
			{
				if (!wait_idle(r, ctx, 20000, true)) timeouts++;
				for (u64 st : cmds[stop_cmd[s]].memory_state) apply_state(st);
				ctrl->put = stops[s + 1];
			}
		}

		if (!wait_idle(r, ctx, 60000, false)) timeouts++;
		Sleep(2); // let the last flip's on_frame_end finish
		s_rloop.store(-1, std::memory_order_release);
		flips[loop] = s_rflip.load();
		loop_end[loop] = { qpc_now(), thread_cycles(rsx_h), r->performance_counters.idle_time.load() };

		if (flips[loop] == 0)
		{
			// Capture held no flip method (game flipped by syscall): flip manually, as upstream
			r->request_emu_flip(ctx, ctx->current_display_buffer);
		}
		std::printf("[cpu-replay] rsx: loop %d done: flips=%u wall=%.2f ms\n", loop, flips[loop],
			(loop_end[loop].qpc - s_samples[loop][0].qpc) * 1000.0 / qpc_hz());
		std::fflush(stdout);
	}

	const double qhz = static_cast<double>(qpc_hz());
	const double tsc_hz = (__rdtsc() - tsc0) * qhz / std::max<double>(1.0, static_cast<double>(qpc_now() - q0));
	const double cyc_ms = 1000.0 / tsc_hz;

	const std::string txt = std::string(path) + ".replay.txt";
	const std::string csv = std::string(path) + ".replay.csv";
	std::FILE* ft = std::fopen(txt.c_str(), "w");
	std::FILE* fc = std::fopen(csv.c_str(), "w");
	auto out = [&](const char* fmt, auto... a)
	{
		std::printf(fmt, a...);
		if (ft) std::fprintf(ft, fmt, a...);
	};

	out("[cpu-replay] rsx replay: %s\n", path);
	out("mode=%s loops=%d frames=%u cmds=%zu mem_applied=%llu (%.1f MB) mem_missing=%llu state_mid_packet=%llu timeouts=%u tsc=%.0f MHz\n",
		exact ? "exact" : "fast", loops, frames, cmds.size(), static_cast<unsigned long long>(n_apply), apply_bytes / 1048576.0,
		static_cast<unsigned long long>(n_miss), static_cast<unsigned long long>(n_state_mid), timeouts, tsc_hz / 1e6);
	out("\nper loop (loop 0 = warm-up: shader/pipeline compiles, texture uploads)\n");
	out("loop flips   wall_ms  rsx_cpu_ms  rsx_idle_ms\n");
	double sw = 0, sc = 0, si = 0;
	int nmeas = 0;
	for (int l = 0; l < loops; l++)
	{
		const sample& a = s_samples[l][0];
		const sample& b = loop_end[l];
		const double w = (b.qpc - a.qpc) * 1000.0 / qhz, c = (b.cyc - a.cyc) * cyc_ms, i = (b.idle_us - a.idle_us) / 1000.0;
		out("%4d %5u %9.2f %11.2f %12.2f\n", l, flips[l], w, c, i);
		if (l > 0 || loops == 1) { sw += w; sc += c; si += i; nmeas++; }
	}
	if (nmeas)
		out("mean(loops>=1) wall=%.2f ms  rsx_cpu=%.2f ms  idle=%.2f ms  => %.2f ms rsx cpu/frame\n",
			sw / nmeas, sc / nmeas, si / nmeas, frames ? sc / nmeas / frames : 0.0);

	out("\nper frame (mean over loops>=1; game = live samples at capture time)\n");
	out("frame  wall_ms  rsx_cpu_ms  idle_ms | game_wall_ms game_rsx_cpu_ms game_idle_ms\n");
	if (fc) std::fprintf(fc, "loop,frame,wall_ms,rsx_cpu_ms,idle_ms\n");
	const double cap_tsc_hz = (s_rstates.size() > 1 && s_rstates.back().qpc > s_rstates[0].qpc)
		? (s_rstates.back().tsc - s_rstates[0].tsc) * static_cast<double>(h.qpc_hz) / (s_rstates.back().qpc - s_rstates[0].qpc) : tsc_hz;
	for (u32 f = 1; f <= frames; f++)
	{
		double w = 0, c = 0, i = 0;
		int n = 0;
		for (int l = 0; l < loops; l++)
		{
			if (flips[l] < f) continue;
			const sample& a = s_samples[l][f - 1];
			const sample& b = s_samples[l][f];
			const double fw = (b.qpc - a.qpc) * 1000.0 / qhz, fcy = (b.cyc - a.cyc) * cyc_ms, fi = (b.idle_us - a.idle_us) / 1000.0;
			if (fc) std::fprintf(fc, "%d,%u,%.4f,%.4f,%.4f\n", l, f, fw, fcy, fi);
			if (l > 0 || loops == 1) { w += fw; c += fcy; i += fi; n++; }
		}
		const fstate& ga = s_rstates[f - 1];
		const fstate& gb = s_rstates[std::min<usz>(f, s_rstates.size() - 1)];
		out("%5u %8.2f %11.2f %8.2f | %12.2f %15.2f %12.2f\n", f, n ? w / n : 0.0, n ? c / n : 0.0, n ? i / n : 0.0,
			(gb.qpc - ga.qpc) * 1000.0 / h.qpc_hz, (gb.cyc - ga.cyc) * 1000.0 / cap_tsc_hz, (gb.idle_us - ga.idle_us) / 1000.0);
	}
	out("\nNote: frame 1 of each loop includes the dmactl kick; with mode=exact, idle_ms counts the per-stop drains.\n");

	if (ft) std::fclose(ft);
	if (fc) std::fclose(fc);
	std::fflush(stdout);
	std::fprintf(stderr, "[cpu-replay] rsx: replay done -> %s\n", txt.c_str());
	std::fflush(stderr);
	TerminateProcess(GetCurrentProcess(), 0);
	for (;;) Sleep(1000);
}
