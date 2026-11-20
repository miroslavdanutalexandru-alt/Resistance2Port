// hd_ui_game_r2.h: the Resistance 2 backend of HDUIPack.exe (hd_ui_pack.cpp, the same file in all three ports).
// Everything game-specific is behind this header: where the game files are, how the archives are read, which textures
// are menu / HUD art, the key the game's texture upload computes for each (r2_hd_ui.h, shared with the RPCS3 Vulkan
// texture cache's RSX_HD_UI path) and how a texture's top level is decoded.
//
// The game's textures: USRDIR\packed\**\*.psarc, members built/tuids/<id>/header.dat + texel.dat (the images the
// Scaleform menus and HUD show through img://). header.dat is IGHW with section 0x5200 of 0x20-byte records
// {u32 offset into the texel data, FORMAT, ADDRESS, CONTROL0, REMAP, FILTER, IMAGE_RECT, CONTROL3}; texel.dat holds
// the texel data in section 0x5300. The game copies the texel bytes to video memory as they are, so the key computed
// here from the archive bytes is the key the upload computes from guest memory.
// Plus the loose images Scaleform movies name (GFx DefineExternalImage: the loading icon, title screen, front end,
// button prompts...): see "loose images" below.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "r2_hd_ui.h"
#include "mods_psarc.h"
#include "miniz.h"

namespace hdgame
{
	namespace fs = std::filesystem;

	constexpr const char* game_name = "Resistance 2";

	// Vulkan creates DXT textures of any size, so the HD size need not be whole 4x4 blocks
	constexpr bool dxt_whole_blocks = false;
	constexpr uint32_t min_side = 8;   // the runtime only keys textures at least 8x8

	inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

	// IGHW container (Insomniac asset file): {u32 'IGHW', u32 version, u32 sections, ...}, section table at +32 of
	// 16-byte entries {u32 id, u32 offset, u32 count (0x10000000 flag: an array of `size`-byte records), u32 size}
	inline bool ighw_section(const std::vector<uint8_t>& b, uint32_t id, uint32_t& off, uint32_t& count)
	{
		if (b.size() < 48 || std::memcmp(b.data(), "IGHW", 4)) return false;
		const uint32_t n = be32(&b[8]);
		if (n > 256 || 32 + size_t(n) * 16 > b.size()) return false;
		for (uint32_t i = 0; i < n; ++i)
		{
			const uint8_t* e = &b[32 + i * 16];
			if (be32(e) != id) continue;
			off = be32(e + 4);
			count = be32(e + 8) & 0xFFFFFF;
			return off < b.size();
		}
		return false;
	}

	// One texture as the RSX sees it (the texture registers the asset stores)
	struct TexInfo
	{
		uint16_t w = 0, h = 0;
		uint32_t gcm = 0;       // with LN / UN
		uint32_t fmt = 0;       // without
		uint32_t mips = 1;      // what the game uploads (exact mip count)
		uint32_t pitch = 0;
		size_t data_off = 0;    // into texel.dat
		uint64_t key = 0;
		std::string skip;       // why it is not packed ("" = it is)
	};

	// Parses a header.dat / texel.dat pair: one TexInfo per texture record, keyed from texel.dat
	inline std::vector<TexInfo> parse_textures(const std::vector<uint8_t>& header, const std::vector<uint8_t>& texel)
	{
		std::vector<TexInfo> outv;
		uint32_t roff = 0, rcount = 0, toff = 0, tcount = 0;
		if (!ighw_section(header, 0x5200, roff, rcount) || !ighw_section(texel, 0x5300, toff, tcount)) return outv;
		for (uint32_t i = 0; i < rcount && roff + size_t(i + 1) * 0x20 <= header.size(); ++i)
		{
			const uint8_t* r = &header[roff + size_t(i) * 0x20];
			const uint32_t off = be32(r), format = be32(r + 4), rect = be32(r + 24), ctrl3 = be32(r + 28);
			TexInfo t;
			t.w = uint16_t(rect >> 16);
			t.h = uint16_t(rect & 0xFFFF);
			t.gcm = (format >> 8) & 0xFF;
			t.fmt = hdui::base_format(t.gcm);
			const uint32_t dim = (format >> 4) & 0xF, depth = ctrl3 >> 20;
			const bool cube = (format & 4) != 0, no_border = (format & 8) != 0;
			t.pitch = ctrl3 & 0xFFFFF;
			t.data_off = size_t(toff) + off;
			if (!hdui::handled_format(t.fmt)) t.skip = hdui::sfmt("format 0x%02x", t.gcm);
			else if (dim != 2 || cube || depth > 1) t.skip = "not a 2D texture";
			else if (!hdui::is_dxt(t.fmt) && !no_border) t.skip = "has border texels";
			else if (t.w < min_side || t.h < min_side) t.skip = "smaller than 8x8";
			if (t.skip.empty())
			{
				t.mips = hdui::exact_mip_count(t.fmt, t.w, t.h, format >> 16);
				const size_t avail = t.data_off < texel.size() ? texel.size() - t.data_off : 0;
				const auto lay = hdui::layout_2d(texel.data() + std::min(t.data_off, texel.size()), avail, t.w, t.h, t.gcm, t.mips, t.pitch);
				bool ok = true;
				for (const auto& s : lay) ok = ok && s.data;
				if (!ok) t.skip = "texel data truncated";
				else t.key = hdui::hash(lay, t.w, t.h, t.fmt, t.mips);
			}
			outv.push_back(t);
		}
		return outv;
	}

	struct Source
	{
		TexInfo tex;
		std::string archive, header, texel;   // archive path, member names (a loose image: its file, both)
		bool image = false;                   // a loose image (Scaleform external image), not a tuids texture
	};

	// Every packable texture of the archives under `usrdir`\packed, by key (the first archive wins a duplicate)
	struct Scan
	{
		std::map<uint64_t, Source> textures;
		std::map<std::string, unsigned> skipped;
		unsigned assets = 0, archives = 0;
		unsigned images = 0;   // loose image files read
	};

	inline std::string lower_ascii(std::string s)
	{
		for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	// ---- loose images. Some Scaleform movies (loadicon.swf: the loading icon; title screen, front end, button
	// prompts...) do not draw tuids textures: GFx tag 1001 (DefineExternalImage) names a picture file beside the movie
	// (/scaleform/LoadIcon_I6.tga), which the game's GFxImageLoader decodes and uploads itself. A TGA is keyed by its
	// top level in canonical A,R,G,B (hdui::image_key: the runtime finds it in whatever layout the loader used); a DDS
	// (DXT) like a texture of that format, linear, with the file's levels.

	// The picture files the movies name: GFx tag 1001 {u16 id, u16 format, u16 width, u16 height, u8 len + export
	// name, u8 len + file name}. CWS / CFX bodies are zlib.
	inline std::vector<std::string> swf_external_images(const std::vector<uint8_t>& f)
	{
		std::vector<std::string> out;
		if (f.size() < 8) return out;
		const bool z = (f[0] == 'C' && (f[1] == 'W' || f[1] == 'F'));
		const bool plain = (f[0] == 'F' && f[1] == 'W') || (f[0] == 'G' && f[1] == 'F');
		if ((!z && !plain) || f[2] != (f[1] == 'W' ? 'S' : 'X')) return out;
		std::vector<uint8_t> d;
		if (z)
		{
			const uint32_t len = uint32_t(f[4]) | (uint32_t(f[5]) << 8) | (uint32_t(f[6]) << 16) | (uint32_t(f[7]) << 24);
			if (len < 8 || len > (64u << 20)) return out;
			d.assign(f.begin(), f.begin() + 8);
			d.resize(len);
			mz_ulong dl = len - 8;
			if (mz_uncompress(d.data() + 8, &dl, f.data() + 8, mz_ulong(f.size() - 8)) != MZ_OK) return out;
			d.resize(8 + dl);
		}
		else d = f;
		if (d.size() < 9) return out;
		const uint32_t nb = d[8] >> 3;
		size_t o = 8 + (5 + 4 * nb + 7) / 8 + 4;
		while (o + 2 <= d.size())
		{
			const uint32_t h = uint32_t(d[o] | (d[o + 1] << 8));
			o += 2;
			const uint32_t tag = h >> 6;
			size_t len = h & 63;
			if (len == 63)
			{
				if (o + 4 > d.size()) break;
				len = size_t(d[o]) | (size_t(d[o + 1]) << 8) | (size_t(d[o + 2]) << 16) | (size_t(d[o + 3]) << 24);
				o += 4;
			}
			if (o + len > d.size() || tag == 0) break;
			if (tag == 1001 && len >= 10)
			{
				const uint8_t* b = &d[o];
				const size_t el = b[8];
				if (9 + el < len)
				{
					const size_t fl = b[9 + el];
					if (10 + el + fl <= len) out.emplace_back(reinterpret_cast<const char*>(b + 10 + el), fl);
				}
			}
			o += len;
		}
		return out;
	}

	// The member a movie's file name refers to: same folder, any case; the disc has some as <name>.tga
	inline std::string resolve_image(const std::map<std::string, std::string>& lower_names, const std::string& movie, const std::string& file)
	{
		const size_t slash = movie.find_last_of('/');
		const std::string dir = lower_ascii(slash == std::string::npos ? std::string() : movie.substr(0, slash + 1));
		const std::string f = lower_ascii(file);
		for (const std::string& c : { dir + f, dir + f + ".tga" })
		{
			const auto it = lower_names.find(c);
			if (it != lower_names.end()) return it->second;
		}
		return {};
	}

	// A loose image file -> its texture: `pixels` gets the bytes its key is computed from (canonical A,R,G,B for a
	// TGA; the DXT levels of a DDS)
	inline bool parse_image(const std::string& name, const std::vector<uint8_t>& file, TexInfo& t, std::vector<uint8_t>& pixels)
	{
		t = TexInfo{};
		const std::string ln = lower_ascii(name);
		if (ln.size() > 4 && !ln.compare(ln.size() - 4, 4, ".dds"))
		{
			uint32_t hdr[32];
			if (file.size() < 128 || (std::memcpy(hdr, file.data(), 128), hdr[0] != 0x20534444))
			{
				t.skip = "loose image: not a DDS file";
				return false;
			}
			const uint32_t cc = (hdr[20] & 4) ? hdr[21] : 0;
			t.fmt = cc == 0x31545844 ? hdui::GCM_DXT1 : (cc == 0x32545844 || cc == 0x33545844) ? hdui::GCM_DXT23 : (cc == 0x34545844 || cc == 0x35545844) ? hdui::GCM_DXT45 : 0;
			if (!t.fmt)
			{
				t.skip = "loose image: DDS not DXT1/3/5";
				return false;
			}
			t.w = uint16_t(hdr[4]);
			t.h = uint16_t(hdr[3]);
			if (hdr[4] > 65535 || hdr[3] > 65535 || t.w < min_side || t.h < min_side)
			{
				t.skip = "smaller than 8x8";
				return false;
			}
			// swizzled-flag DXT: RPCS3 lays it out (and the file stores it) with each level's rows packed
			t.gcm = t.fmt;
			t.mips = hdui::exact_mip_count(t.fmt, t.w, t.h, std::max<uint32_t>(1, (hdr[2] & 0x20000) ? hdr[7] : 1));
			t.pitch = 0;
			pixels.assign(file.begin() + 128, file.end());
			const auto lay = hdui::layout_2d(pixels.data(), pixels.size(), t.w, t.h, t.gcm, t.mips, t.pitch);
			for (const auto& s : lay)
				if (!s.data)
				{
					t.skip = "texel data truncated";
					return false;
				}
			t.key = hdui::hash(lay, t.w, t.h, t.fmt, t.mips);
			return true;
		}
		uint32_t w = 0, h = 0;
		std::string why;
		if (!hdui::decode_tga(file.data(), file.size(), w, h, pixels, &why))
		{
			t.skip = "loose image: " + why;
			return false;
		}
		t.w = uint16_t(w);
		t.h = uint16_t(h);
		if (w < min_side || h < min_side)
		{
			t.skip = "smaller than 8x8";
			return false;
		}
		t.fmt = hdui::GCM_A8R8G8B8;
		t.gcm = hdui::GCM_A8R8G8B8 | hdui::GCM_LN;
		t.mips = 1;
		t.pitch = w * 4;
		t.key = hdui::image_key(pixels.data(), w, h);
		return true;
	}

	// The loose images the Scaleform movies of one archive name (member names, each once)
	inline std::vector<std::string> archive_images(const std::string& arc, const std::vector<std::string>& names, std::vector<std::string>* missing = nullptr)
	{
		std::map<std::string, std::string> lower_names;
		for (const auto& n : names) lower_names.emplace(lower_ascii(n), n);
		std::vector<std::string> out;
		std::set<std::string> have;
		for (const auto& n : names)
		{
			const std::string ln = lower_ascii(n);
			if (ln.size() < 4 || (ln.compare(ln.size() - 4, 4, ".swf") && ln.compare(ln.size() - 4, 4, ".gfx"))) continue;
			std::vector<uint8_t> f;
			if (!modpsarc::read_member(arc, n, f)) continue;
			for (const auto& file : swf_external_images(f))
			{
				const std::string m = resolve_image(lower_names, n, file);
				if (m.empty())
				{
					if (missing) missing->push_back(n + ": " + file);
					continue;
				}
				if (have.insert(m).second) out.push_back(m);
			}
		}
		return out;
	}

	// progress(done, total): over the archives
	inline Scan scan_archives(const fs::path& usrdir, const std::function<void(unsigned, unsigned)>& progress)
	{
		Scan sc;
		std::vector<fs::path> arcs;
		std::error_code ec;
		const fs::path packed = usrdir / "packed";
		for (auto it = fs::recursive_directory_iterator(packed, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
			if (it->is_regular_file(ec) && lower_ascii(it->path().extension().string()) == ".psarc") arcs.push_back(it->path());
		// game\ (global assets) first, then the levels in name order
		std::sort(arcs.begin(), arcs.end(), [&](const fs::path& a, const fs::path& b)
		{
			const bool ga = lower_ascii(a.parent_path().filename().string()) == "game", gb = lower_ascii(b.parent_path().filename().string()) == "game";
			return ga != gb ? ga : a < b;
		});
		for (size_t ai = 0; ai < arcs.size(); ++ai)
		{
			if (progress) progress(unsigned(ai), unsigned(arcs.size()));
			const std::string arc = arcs[ai].string();
			std::vector<std::string> names;
			if (!modpsarc::list_members(arc, names)) continue;
			sc.archives++;
			std::set<std::string> have(names.begin(), names.end());
			for (const auto& n : names)
			{
				// built/tuids/<3 hex>/<16 hex>/header.dat (fonts use font_header.dat: they are not drawn from these images)
				const std::string ln = lower_ascii(n);
				if (ln.find("tuids/") == std::string::npos) continue;
				if (ln.size() < 11 || ln.compare(ln.size() - 11, 11, "/header.dat")) continue;
				const std::string tx = n.substr(0, n.size() - 10) + "texel.dat";
				if (!have.count(tx)) continue;
				std::vector<uint8_t> hb, tb;
				if (!modpsarc::read_member(arc, n, hb) || !modpsarc::read_member(arc, tx, tb)) continue;
				const auto texs = parse_textures(hb, tb);
				if (texs.empty()) continue;
				sc.assets++;
				for (const auto& t : texs)
				{
					if (!t.skip.empty())
					{
						sc.skipped[t.skip]++;
						continue;
					}
					if (sc.textures.count(t.key)) continue;
					sc.textures[t.key] = Source{ t, arc, n, tx };
				}
			}
			for (const auto& m : archive_images(arc, names))
			{
				std::vector<uint8_t> f, px;
				if (!modpsarc::read_member(arc, m, f)) continue;
				sc.images++;
				TexInfo t;
				if (!parse_image(m, f, t, px))
				{
					sc.skipped[t.skip]++;
					continue;
				}
				if (sc.textures.count(t.key)) continue;
				Source s{ t, arc, m, m };
				s.image = true;
				sc.textures[t.key] = s;
			}
		}
		if (progress) progress(unsigned(arcs.size()), unsigned(arcs.size()));
		return sc;
	}

	// The bytes a source's key was computed from: the texel member of a texture; the decoded pixels of a loose image
	inline bool read_texel(const Source& s, std::vector<uint8_t>& tb)
	{
		if (s.image)
		{
			std::vector<uint8_t> f;
			TexInfo t;
			return modpsarc::read_member(s.archive, s.texel, f) && parse_image(s.texel, f, t, tb) && t.key == s.tex.key;
		}
		return modpsarc::read_member(s.archive, s.texel, tb) && s.tex.data_off < tb.size();
	}

	// Block rows of a DXT texture are this many bytes apart (RPCS3's layout: swizzled DXT is stored linear)
	inline uint32_t dxt_row_bytes(const TexInfo& t)
	{
		const auto lay = hdui::layout_2d(nullptr, 0, t.w, t.h, t.gcm, 1, t.pitch);
		return lay[0].pitch_in_block * hdui::block_bytes(t.fmt);
	}

	// Level 0 of an A8R8G8B8 texture -> RGBA (w * 4 bytes a row): bytes A, R, G, B; swizzled = Morton order (x bit
	// first) inside the power-of-two square
	inline void decode_argb(const uint8_t* d, const TexInfo& t, uint8_t* rgba)
	{
		const uint32_t w = t.w, h = t.h;
		const bool swz = !(t.gcm & hdui::GCM_LN);
		const uint32_t rowb = t.pitch && !swz ? t.pitch : w * 4;
		uint32_t lw = 0, lh = 0;
		while ((1u << lw) < w) ++lw;
		while ((1u << lh) < h) ++lh;
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x)
			{
				size_t o;
				if (swz)
				{
					size_t m = 0;
					uint32_t bit = 0;
					for (uint32_t i = 0; i < std::max(lw, lh); ++i)
					{
						if (i < lw) m |= size_t((x >> i) & 1) << bit++;
						if (i < lh) m |= size_t((y >> i) & 1) << bit++;
					}
					o = m * 4;
				}
				else o = size_t(y) * rowb + size_t(x) * 4;
				uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
				p[0] = d[o + 1];
				p[1] = d[o + 2];
				p[2] = d[o + 3];
				p[3] = d[o + 0];
			}
	}

	inline fs::path find_usrdir(const fs::path& root, const fs::path& given)
	{
		std::error_code ec;
		std::vector<fs::path> cand;
		if (!given.empty()) cand = { given / "USRDIR", given / "PS3_GAME" / "USRDIR", given };
		else cand = { root / "gamefiles" / "PS3_GAME" / "USRDIR", root / "game" / "PS3_GAME" / "USRDIR", root / "PS3_GAME" / "USRDIR", root / "game" / "USRDIR" };
		for (const auto& c : cand)
			if (fs::is_directory(c / "packed", ec)) return c;
		return {};
	}

	inline std::string text_no_game() { return "the game files were not found (gamefiles\\PS3_GAME\\USRDIR\\packed next to the launcher): run Setup first"; }
	inline std::string text_no_textures(const fs::path& usrdir, unsigned archives)
	{
		return hdui::sfmt("no menu / HUD textures were found in %s\\packed (%u archives read)", usrdir.string().c_str(), archives);
	}
}
