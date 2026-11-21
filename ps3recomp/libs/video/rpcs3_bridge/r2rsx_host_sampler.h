// R3PORT perf round 8: RSX_HOST_SAMPLE=1 (default off), a host sampling profiler for rsx::thread.
// Shared verbatim by R2 and R3 (libs/video/rpcs3_bridge). Started once from the bridge after the renderer exists.
//
// Every RSX_HOST_SAMPLE_MS (default 1) ms the sampler thread suspends rsx::thread, reads its context and unwinds the
// host stack (at most 64 frames) over a cached copy of each module's .pdata, then resumes it. Nothing allocates or
// takes a lock while the thread is suspended (same design as PPU_PROF_SAMPLE in runtime/ppu/ppu_loader.cpp): counts
// go to static open-addressing tables. Every RSX_HOST_SAMPLE_SECS (default 10) s it prints, on stdout, prefix
// [rsx-sample]:
//   - share per leaf module (game exe, nvoglv64/amdvlk/igvk driver, vulkan-1, ntdll, ...);
//   - top RSX_HOST_SAMPLE_TOP (default 30) self functions (leaf, outer function and innermost inlined function);
//   - top inclusive functions (on the stack, counted once per sample);
//   - "into-module" rows: samples whose leaf is outside the game exe, charged to the innermost game-exe frame
//     (which vkCmd* / driver / OS call site is hot);
//   - top source lines of the self samples.
// Symbols are resolved at print time only, on the sampler thread, with DbgHelp (PDB next to the exe, loaded lazily
// on the first print: the R3 PDB is ~0.8 GB, so the first window can take a few seconds to print).
// RSX_HOST_SAMPLE_SYMS=0 prints module+RVA only. RSX_HOST_SAMPLE_REPEAT=N stops after N windows (0 = until exit).
// Cost to the sampled thread: one suspend/resume + unwind per sample (~20-40 us at 1 kHz = 2-4 % of rsx::thread).
#pragma once

#ifdef _WIN32
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace rsx_host_sample
{
	struct mod_t { DWORD64 base, end; const RUNTIME_FUNCTION* pd; DWORD n; HMODULE h; };
	enum { max_mods = 512, max_depth = 64, nf = 1 << 15, nr = 1 << 16 };

	struct state_t
	{
		mod_t mods[max_mods];
		int mod_n = 0;
		HMODULE hm_tmp[max_mods];
		mod_t nm_tmp[max_mods];

		// function start -> self / inclusive counts
		DWORD64 fkey[nf];
		uint32_t fself[nf], fincl[nf];
		// exact leaf RIP -> count (inline frames and source lines)
		DWORD64 rkey[nr];
		uint32_t rcnt[nr];
		// innermost game-exe frame (function start) of samples whose leaf is outside the exe -> count
		DWORD64 bkey[nf];
		uint32_t bcnt[nf];
	};

	inline state_t& st()
	{
		static state_t* s = new state_t{};
		return *s;
	}

	inline int env_int(const char* n, int d)
	{
		const char* e = std::getenv(n);
		return e && *e ? std::atoi(e) : d;
	}

	inline void refresh_mods()
	{
		auto& s = st();
		DWORD need = 0;
		if (!K32EnumProcessModules(GetCurrentProcess(), s.hm_tmp, sizeof(s.hm_tmp), &need)) return;
		int n = static_cast<int>(need / sizeof(HMODULE)), k = 0;
		if (n > max_mods) n = max_mods;
		for (int i = 0; i < n; i++)
		{
			HMODULE h = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(s.hm_tmp[i]), &h) || !h) continue;
			const BYTE* b = reinterpret_cast<const BYTE*>(h);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
			const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
			mod_t m{ reinterpret_cast<DWORD64>(b), reinterpret_cast<DWORD64>(b) + nt->OptionalHeader.SizeOfImage,
				dd.VirtualAddress ? reinterpret_cast<const RUNTIME_FUNCTION*>(b + dd.VirtualAddress) : nullptr,
				dd.VirtualAddress ? static_cast<DWORD>(dd.Size / sizeof(RUNTIME_FUNCTION)) : 0u, h };
			int j = k++;
			for (; j > 0 && s.nm_tmp[j - 1].base > m.base; j--) s.nm_tmp[j] = s.nm_tmp[j - 1];
			s.nm_tmp[j] = m;
		}
		for (int i = 0; i < s.mod_n; i++) FreeLibrary(s.mods[i].h);
		std::memcpy(s.mods, s.nm_tmp, sizeof(mod_t) * k);
		s.mod_n = k;
	}

	// Lock-free RtlLookupFunctionEntry over the cached images. *mod = -1 when pc is in no known image;
	// nullptr with *mod >= 0 is a leaf function (no unwind data).
	inline const RUNTIME_FUNCTION* lookup(DWORD64 pc, int* mod)
	{
		auto& s = st();
		*mod = -1;
		int lo = 0, hi = s.mod_n;
		while (lo < hi) { const int mid = (lo + hi) / 2; if (s.mods[mid].base <= pc) lo = mid + 1; else hi = mid; }
		if (!lo || pc >= s.mods[lo - 1].end) return nullptr;
		const mod_t& m = s.mods[lo - 1];
		*mod = lo - 1;
		const DWORD rva = static_cast<DWORD>(pc - m.base);
		DWORD l = 0, h = m.n;
		while (l < h)
		{
			const DWORD mid = (l + h) / 2;
			const RUNTIME_FUNCTION* f = &m.pd[mid];
			if (rva < f->BeginAddress) h = mid;
			else if (rva >= f->EndAddress) l = mid + 1;
			else
			{
				for (int c = 0; c < 8 && (f->UnwindData & 1); c++)
					f = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + (f->UnwindData & ~1u));
				return f;
			}
		}
		return nullptr;
	}

	inline uint32_t* bump(DWORD64* keys, uint32_t* cnt, uint32_t n, DWORD64 k)
	{
		uint32_t i = static_cast<uint32_t>(k * 0x9E3779B97F4A7C15ull >> 40) & (n - 1);
		for (uint32_t p = 0; p < n; p++, i = (i + 1) & (n - 1))
		{
			if (keys[i] == k) { cnt[i]++; return &cnt[i]; }
			if (!keys[i]) { keys[i] = k; cnt[i] = 1; return &cnt[i]; }
		}
		return nullptr;
	}

	inline void bump_incl(DWORD64 k)
	{
		auto& s = st();
		uint32_t i = static_cast<uint32_t>(k * 0x9E3779B97F4A7C15ull >> 40) & (nf - 1);
		for (uint32_t p = 0; p < nf; p++, i = (i + 1) & (nf - 1))
		{
			if (s.fkey[i] == k) { s.fincl[i]++; return; }
			if (!s.fkey[i]) { s.fkey[i] = k; s.fincl[i] = 1; return; }
		}
	}

	inline void bump_self(DWORD64 k)
	{
		auto& s = st();
		uint32_t i = static_cast<uint32_t>(k * 0x9E3779B97F4A7C15ull >> 40) & (nf - 1);
		for (uint32_t p = 0; p < nf; p++, i = (i + 1) & (nf - 1))
		{
			if (s.fkey[i] == k) { s.fself[i]++; return; }
			if (!s.fkey[i]) { s.fkey[i] = k; s.fself[i] = 1; return; }
		}
	}

	struct sym_t
	{
		HANDLE proc = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x52535853u)); // unique fake handle, not the process
		bool init = false, on = true;
		std::unordered_map<DWORD64, bool> loaded; // module base -> symbols loaded

		bool ensure(DWORD64 addr)
		{
			if (!on) return false;
			if (!init)
			{
				char exe[MAX_PATH] = {};
				GetModuleFileNameA(nullptr, exe, MAX_PATH);
				if (char* p = std::strrchr(exe, '\\')) *p = 0;
				SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
				if (!SymInitialize(proc, exe, FALSE)) { on = false; return false; }
				init = true;
			}
			HMODULE m = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(addr), &m) || !m) return false;
			const DWORD64 base = reinterpret_cast<DWORD64>(m);
			auto it = loaded.find(base);
			if (it != loaded.end()) return it->second;
			char path[MAX_PATH] = {};
			GetModuleFileNameA(m, path, MAX_PATH);
			MODULEINFO mi{};
			K32GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi));
			const bool ok = SymLoadModuleEx(proc, nullptr, path, nullptr, base, mi.SizeOfImage, nullptr, 0) != 0 || GetLastError() == ERROR_SUCCESS;
			loaded[base] = ok;
			return ok;
		}

		// outer function name; empty if unknown
		std::string name(DWORD64 addr)
		{
			if (!ensure(addr)) return {};
			alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
			auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
			std::memset(si, 0, sizeof(SYMBOL_INFO));
			si->SizeOfStruct = sizeof(SYMBOL_INFO);
			si->MaxNameLen = 511;
			DWORD64 disp = 0;
			return SymFromAddr(proc, addr, &disp, si) ? std::string(si->Name) : std::string{};
		}

		// innermost inlined function at addr (falls back to the outer name), plus its source line
		std::string inline_name(DWORD64 addr, std::string* line)
		{
			if (!ensure(addr)) return {};
			alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
			auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
			std::memset(si, 0, sizeof(SYMBOL_INFO));
			si->SizeOfStruct = sizeof(SYMBOL_INFO);
			si->MaxNameLen = 511;
			DWORD64 disp = 0;
			std::string r;
			ULONG ctx = 0, idx = 0;
			bool have_ctx = false;
			if (SymAddrIncludeInlineTrace(proc, addr) && SymQueryInlineTrace(proc, addr, 0, addr, addr, &ctx, &idx))
			{
				have_ctx = true;
				if (SymFromInlineContext(proc, addr, ctx, &disp, si)) r = si->Name;
			}
			if (r.empty() && SymFromAddr(proc, addr, &disp, si)) r = si->Name;
			if (line)
			{
				IMAGEHLP_LINE64 ln{};
				ln.SizeOfStruct = sizeof(ln);
				DWORD d32 = 0;
				const BOOL ok = have_ctx ? SymGetLineFromInlineContext(proc, addr, ctx, 0, &d32, &ln) : SymGetLineFromAddr64(proc, addr, &d32, &ln);
				if (ok && ln.FileName)
				{
					const char* f = ln.FileName;
					for (const char* p = ln.FileName; *p; p++) if (*p == '\\' || *p == '/') f = p + 1;
					*line = std::string(f) + ":" + std::to_string(ln.LineNumber);
				}
			}
			return r;
		}
	};

	inline std::string mod_name(DWORD64 addr, DWORD64* base)
	{
		HMODULE m = nullptr;
		char mn[MAX_PATH] = "?";
		*base = 0;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(addr), &m) && m)
		{
			GetModuleFileNameA(m, mn, sizeof(mn));
			*base = reinterpret_cast<DWORD64>(m);
		}
		const char* bn = std::strrchr(mn, '\\');
		return bn ? bn + 1 : mn;
	}

	inline HANDLE find_rsx_thread(DWORD* tid_out)
	{
		const DWORD pid = GetCurrentProcessId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return nullptr;
		HANDLE found = nullptr;
		THREADENTRY32 te{};
		te.dwSize = sizeof(te);
		for (BOOL ok = Thread32First(snap, &te); ok && !found; ok = Thread32Next(snap, &te))
		{
			if (te.th32OwnerProcessID != pid) continue;
			HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
			if (!h) continue;
			PWSTR d = nullptr;
			if (SUCCEEDED(GetThreadDescription(h, &d)) && d && wcsstr(d, L"rsx::thread"))
			{
				found = h;
				*tid_out = te.th32ThreadID;
				h = nullptr;
			}
			if (d) LocalFree(d);
			if (h) CloseHandle(h);
		}
		CloseHandle(snap);
		return found;
	}

	struct row_t { std::string name; uint64_t a = 0, b = 0; };

	inline void print_rows(std::string& out, const char* tag, std::vector<row_t>& rows, uint32_t total, int top, bool two)
	{
		std::sort(rows.begin(), rows.end(), [](const row_t& x, const row_t& y) { return x.a > y.a; });
		char ln[1024];
		for (int r = 0; r < top && r < static_cast<int>(rows.size()); r++)
		{
			if (two)
				std::snprintf(ln, sizeof(ln), "[rsx-sample] %s %5.1f%% self %5.1f%%  %s\n", tag, 100.0 * rows[r].a / total, 100.0 * rows[r].b / total, rows[r].name.c_str());
			else
				std::snprintf(ln, sizeof(ln), "[rsx-sample] %s %5.1f%%  %s\n", tag, 100.0 * rows[r].a / total, rows[r].name.c_str());
			out += ln;
		}
	}

	inline void sampler_main()
	{
		SetThreadDescription(GetCurrentThread(), L"rsx host sampler");
		auto& s = st();
		int secs = env_int("RSX_HOST_SAMPLE_SECS", 10), ms = env_int("RSX_HOST_SAMPLE_MS", 1), top = env_int("RSX_HOST_SAMPLE_TOP", 30);
		const int repeat = env_int("RSX_HOST_SAMPLE_REPEAT", 0);
		sym_t sym;
		sym.on = env_int("RSX_HOST_SAMPLE_SYMS", 1) != 0;
		secs = (std::max)(secs, 1); ms = (std::max)(ms, 1); top = (std::max)(top, 1);

		DWORD tid = 0;
		HANDLE target = nullptr;
		for (int tries = 0; !(target = find_rsx_thread(&tid)); tries++)
		{
			if (tries == 600) { std::fprintf(stdout, "[rsx-sample] rsx::thread not found after 10 min, sampler off\n"); std::fflush(stdout); return; }
			Sleep(1000);
		}
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		std::fprintf(stdout, "[rsx-sample] sampling rsx::thread (tid %lu) every %d ms, top %d every %d s, symbols %s "
			"(RSX_HOST_SAMPLE / _MS / _SECS / _TOP / _REPEAT / _SYMS)\n", static_cast<unsigned long>(tid), ms, top, secs, sym.on ? "on" : "off");
		std::fflush(stdout);
		timeBeginPeriod(1);
		// high-resolution timer: Sleep(1) wakes every ~2 ms
		HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

		const DWORD64 exe_base = reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
		for (int win = 1; !repeat || win <= repeat; win++)
		{
			refresh_mods();
			int exe_mod = -1;
			for (int i = 0; i < s.mod_n; i++) if (s.mods[i].base == exe_base) exe_mod = i;
			const ULONGLONG t_start = GetTickCount64(), end = t_start + static_cast<ULONGLONG>(secs) * 1000;
			uint32_t total = 0, lost = 0;
			while (GetTickCount64() < end)
			{
				if (timer)
				{
					LARGE_INTEGER due;
					due.QuadPart = -static_cast<LONGLONG>(ms) * 10000;
					if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
					else Sleep(static_cast<DWORD>(ms));
				}
				else Sleep(static_cast<DWORD>(ms));
				if (SuspendThread(target) == static_cast<DWORD>(-1)) { CloseHandle(target); if (timer) CloseHandle(timer); timeEndPeriod(1); return; }
				CONTEXT c;
				c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
				DWORD64 leaf_pc = 0, seen[max_depth], boundary = 0;
				int ns = 0, leaf_mod = -1;
				bool got = false;
				if (GetThreadContext(target, &c))
				{
					got = true;
					leaf_pc = c.Rip;
					for (int d = 0; d < max_depth && c.Rip; d++)
					{
						int mi = -1;
						const RUNTIME_FUNCTION* f = lookup(c.Rip, &mi);
						if (d == 0) leaf_mod = mi;
						const DWORD64 start = f ? s.mods[mi].base + f->BeginAddress : c.Rip;
						if (f)
						{
							bool dup = false;
							for (int k = 0; k < ns; k++) if (seen[k] == start) { dup = true; break; }
							if (!dup) seen[ns++] = start;
							if (!boundary && d > 0 && mi == exe_mod && leaf_mod != exe_mod) boundary = start;
						}
						if (mi < 0 || (!f && d)) break;
						if (!f) { c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp); c.Rsp += 8; continue; }
						PVOID hd; DWORD64 ef;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, s.mods[mi].base, c.Rip, const_cast<PRUNTIME_FUNCTION>(f), &c, &hd, &ef, nullptr);
					}
				}
				ResumeThread(target);
				if (!got) { lost++; continue; }
				total++;
				// leaf: function start when known, else the raw pc (charged as its own "function")
				bump_self(ns ? seen[0] : leaf_pc);
				for (int k = 0; k < ns; k++) bump_incl(seen[k]);
				bump(s.rkey, s.rcnt, nr, leaf_pc);
				if (boundary) bump(s.bkey, s.bcnt, nf, boundary);
			}
			const double wall = (GetTickCount64() - t_start) / 1000.0;
			std::string out;
			char ln[512];
			if (!total)
			{
				std::snprintf(ln, sizeof(ln), "[rsx-sample] window %d: no samples\n", win);
				out += ln;
			}
			else
			{
				const ULONGLONG t_sym = GetTickCount64();
				std::snprintf(ln, sizeof(ln), "[rsx-sample] window %d: %u samples in %.1f s (%.0f/s, %u failed); %% = share of rsx::thread wall time\n",
					win, total, wall, total / wall, lost);
				out += ln;

				// per leaf module
				std::unordered_map<std::string, uint64_t> by_mod;
				for (uint32_t i = 0; i < nr; i++)
				{
					if (!s.rkey[i]) continue;
					DWORD64 b;
					by_mod[mod_name(s.rkey[i], &b)] += s.rcnt[i];
				}
				std::vector<row_t> rows;
				for (auto& [k, v] : by_mod) rows.push_back({ k, v, 0 });
				print_rows(out, "module", rows, total, 16, false);

				auto fmt_addr = [&](DWORD64 a, bool inl, std::string* line) -> std::string
				{
					DWORD64 b;
					const std::string m = mod_name(a, &b);
					std::string n = inl ? sym.inline_name(a, line) : sym.name(a);
					char t[64];
					std::snprintf(t, sizeof(t), "%s+0x%llX", m.c_str(), static_cast<unsigned long long>(a - b));
					return n.empty() ? std::string(t) : n + "  [" + t + "]";
				};
				auto top_keys = [](const DWORD64* keys, const uint32_t* cnt, uint32_t n, size_t k)
				{
					std::vector<std::pair<uint32_t, DWORD64>> v;
					for (uint32_t i = 0; i < n; i++) if (keys[i] && cnt[i]) v.push_back({ cnt[i], keys[i] });
					const size_t kk = (std::min)(k, v.size());
					std::partial_sort(v.begin(), v.begin() + kk, v.end(), [](auto& x, auto& y) { return x.first > y.first; });
					v.resize(kk);
					return v;
				};

				// self by outer function (merged by name: identical-code-folded / multi-chunk functions)
				{
					std::unordered_map<std::string, row_t> m;
					for (auto& [c, a] : top_keys(s.fkey, s.fself, nf, 300))
					{
						const std::string n = fmt_addr(a, false, nullptr);
						const std::string key = n.substr(0, n.find("  ["));
						auto& r = m[key];
						if (r.name.empty()) r.name = n;
						r.a += c;
					}
					rows.clear();
					for (auto& [k, r] : m) rows.push_back(r);
					print_rows(out, "self", rows, total, top, false);
				}
				// self by innermost inlined function, and by source line
				{
					std::unordered_map<std::string, row_t> m, l;
					for (auto& [c, a] : top_keys(s.rkey, s.rcnt, nr, sym.on ? 1500 : 0))
					{
						std::string line;
						std::string n = sym.inline_name(a, &line);
						if (n.empty()) continue;
						auto& r = m[n];
						if (r.name.empty()) r.name = n;
						r.a += c;
						if (!line.empty())
						{
							auto& q = l[line + "  " + n];
							if (q.name.empty()) q.name = line + "  " + n;
							q.a += c;
						}
					}
					rows.clear();
					for (auto& [k, r] : m) rows.push_back(r);
					print_rows(out, "self-inl", rows, total, top, false);
					rows.clear();
					for (auto& [k, r] : l) rows.push_back(r);
					print_rows(out, "line", rows, total, top, false);
				}
				// inclusive
				{
					std::unordered_map<std::string, row_t> m;
					for (auto& [c, a] : top_keys(s.fkey, s.fincl, nf, 200))
					{
						const std::string n = fmt_addr(a, false, nullptr);
						const std::string key = n.substr(0, n.find("  ["));
						auto& r = m[key];
						if (r.name.empty()) r.name = n;
						r.a = (std::max<uint64_t>)(r.a, c);
						uint32_t self = 0;
						for (uint32_t i = 0; i < nf; i++) if (s.fkey[i] == a) { self = s.fself[i]; break; }
						r.b += self;
					}
					rows.clear();
					for (auto& [k, r] : m) rows.push_back(r);
					print_rows(out, "incl", rows, total, top, true);
				}
				// leaf outside the exe, by the innermost exe frame
				{
					rows.clear();
					for (auto& [c, a] : top_keys(s.bkey, s.bcnt, nf, top))
						rows.push_back({ fmt_addr(a, false, nullptr), c, 0 });
					print_rows(out, "into-module-from", rows, total, top, false);
				}
				std::snprintf(ln, sizeof(ln), "[rsx-sample] window %d end (symbolized in %.2f s)\n", win, (GetTickCount64() - t_sym) / 1000.0);
				out += ln;
			}
			std::fwrite(out.data(), 1, out.size(), stdout);
			std::fflush(stdout);
			std::memset(s.fkey, 0, sizeof(s.fkey)); std::memset(s.fself, 0, sizeof(s.fself)); std::memset(s.fincl, 0, sizeof(s.fincl));
			std::memset(s.rkey, 0, sizeof(s.rkey)); std::memset(s.rcnt, 0, sizeof(s.rcnt));
			std::memset(s.bkey, 0, sizeof(s.bkey)); std::memset(s.bcnt, 0, sizeof(s.bcnt));
		}
		timeEndPeriod(1);
		if (timer) CloseHandle(timer);
		CloseHandle(target);
	}

	// RSX_HOST_SAMPLE=1: start once (detached; it finds rsx::thread by its thread description)
	inline void start_if_enabled()
	{
		static bool started = false;
		const char* e = std::getenv("RSX_HOST_SAMPLE");
		if (started || !e || !*e || *e == '0') return;
		started = true;
		std::thread(sampler_main).detach();
	}
}
#endif
