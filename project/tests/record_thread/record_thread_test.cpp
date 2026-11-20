// R3PORT: RSX_RECORD_THREAD unit test (VKRecordThread.cpp against a logging stub driver).
// The same command script runs with recording off (the reference log), in validation mode 2 and with the
// record thread (mode 1, small ring so it wraps often); every replayed call must match the reference exactly,
// argument data included, although the caller's buffers are clobbered right after each call.
#include "stdafx.h"
#include "Emu/RSX/VK/VulkanAPI.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern std::mutex g_log_mtx;
extern std::vector<std::string> g_log;
extern std::atomic<u64> g_off_main, g_on_main;
void stubs_init();

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; std::printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

template <typename T> static T hnd(u64 v) { return reinterpret_cast<T>(v); }
template <typename T> static void clobber(T& v) { std::memset(static_cast<void*>(&v), 0xA5, sizeof(T)); }

static size_t log_size()
{
	std::lock_guard lock(g_log_mtx);
	return g_log.size();
}

// Log positions right after each drain of the reference run
static std::vector<size_t> g_ref_marks;
static std::vector<size_t> g_marks;
static std::vector<size_t> g_ref_iter; // reference log size after each iteration

static void emit(u32 i, VkCommandBuffer cb)
{
	{
		VkViewport vp[2] = { { float(i), 2.f, 640.f, 360.f, 0.f, 1.f }, { 1.f, float(i & 7), float(i * 3), 9.f, 0.f, 0.5f } };
		vkCmdSetViewport(cb, 0, 1 + (i & 1), vp);
		clobber(vp);
		VkRect2D sc[1] = { { { int32_t(i), 0 }, { 100u + i, 50u } } };
		vkCmdSetScissor(cb, 0, 1, sc);
		clobber(sc);
	}
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hnd<VkPipeline>(0x5000 + (i % 13)));
	{
		u32 pc[16];
		for (u32 k = 0; k < 16; k++) pc[k] = i * 31 + k;
		vkCmdPushConstants(cb, hnd<VkPipelineLayout>(0x77), VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * (1 + (i % 16)), pc);
		clobber(pc);
	}
	{
		VkDescriptorImageInfo ii[3] = { { hnd<VkSampler>(0x100 + i), hnd<VkImageView>(0x200 + i), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ hnd<VkSampler>(0x101), hnd<VkImageView>(0x202 + i), VK_IMAGE_LAYOUT_GENERAL }, {} };
		VkDescriptorBufferInfo bi[1] = { { hnd<VkBuffer>(0x300 + i), 16ull * i, 256 } };
		VkBufferView tv[1] = { hnd<VkBufferView>(0x400 + i) };
		VkWriteDescriptorSet w[3] = {};
		w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii, nullptr, nullptr };
		w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, bi, nullptr };
		w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 3, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, nullptr, nullptr, tv };
		_vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hnd<VkPipelineLayout>(0x77), 1, 3, w);
		w[0].dstSet = w[1].dstSet = w[2].dstSet = hnd<VkDescriptorSet>(0x900 + i);
		vkUpdateDescriptorSets(hnd<VkDevice>(0x1), 3, w, 0, nullptr);
		clobber(ii); clobber(bi); clobber(tv); clobber(w);
	}
	{
		VkDescriptorSet sets[2] = { hnd<VkDescriptorSet>(0x900 + i), hnd<VkDescriptorSet>(0x990) };
		u32 dyn[2] = { i * 256, 512 };
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hnd<VkPipelineLayout>(0x77), 0, 2, sets, (i & 1) ? 2 : 0, dyn);
		clobber(sets); clobber(dyn);
		VkBuffer vb[3] = { hnd<VkBuffer>(0x10), hnd<VkBuffer>(0x11), hnd<VkBuffer>(0x12 + i) };
		VkDeviceSize vo[3] = { 0, 64, 128ull * i };
		vkCmdBindVertexBuffers(cb, 0, 3, vb, vo);
		clobber(vb); clobber(vo);
	}
	vkCmdBindIndexBuffer(cb, hnd<VkBuffer>(0x20), 4ull * i, VK_INDEX_TYPE_UINT16);
	{
		VkImageMemoryBarrier ib[2] = {};
		for (u32 k = 0; k < 2; k++)
		{
			ib[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			ib[k].image = hnd<VkImage>(0x700 + i + k);
			ib[k].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		}
		VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, i, 0 };
		vkCmdPipelineBarrier(cb, 1, 2, 0, 1, &mb, 0, nullptr, 1 + (i & 1), ib);
		clobber(ib); clobber(mb);
	}
	{
		float bc[4] = { float(i), 0.25f, 0.5f, 1.f };
		vkCmdSetBlendConstants(cb, bc);
		clobber(bc);
	}
	vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, i & 0xff);
	vkCmdSetDepthBias(cb, float(i), 0.f, 1.f);
	vkCmdDrawIndexed(cb, 3 * i, 1, i, -int32_t(i & 3), 0);

	if (i % 7 == 0)
	{
		std::vector<u8> data(4 + (i % 3) * 30000);
		for (size_t k = 0; k < data.size(); k++) data[k] = static_cast<u8>(k * 7 + i);
		vkCmdUpdateBuffer(cb, hnd<VkBuffer>(0x30), 0, data.size(), data.data());
		std::fill(data.begin(), data.end(), u8{0});
	}
	if (i % 11 == 0)
	{
		VkClearValue cv[2] = {};
		cv[1].color.float32[0] = float(i);
		VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, hnd<VkRenderPass>(0x40), hnd<VkFramebuffer>(0x41 + i), {}, 2, cv };
		vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
		clobber(cv); clobber(rp);
		VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, i & 3, {} };
		VkClearRect cr = { { { 0, 0 }, { 8, 9u + i } }, 0, 1 };
		vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
		clobber(ca); clobber(cr);
		vkCmdEndRenderPass(cb);
	}
	if (i % 13 == 0)
	{
		VkBufferCopy bcp[2] = { { 0, 0, 16 }, { 16, 32, 64ull + i } };
		vkCmdCopyBuffer(cb, hnd<VkBuffer>(0x50), hnd<VkBuffer>(0x51), 2, bcp);
		clobber(bcp);
		VkBufferImageCopy bic = {};
		bic.bufferOffset = 1024ull * i;
		vkCmdCopyBufferToImage(cb, hnd<VkBuffer>(0x52), hnd<VkImage>(0x53), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
		vkCmdCopyImageToBuffer(cb, hnd<VkImage>(0x53), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hnd<VkBuffer>(0x54), 1, &bic);
		clobber(bic);
		VkClearColorValue ccv = {};
		ccv.float32[3] = float(i);
		VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 1 };
		vkCmdClearColorImage(cb, hnd<VkImage>(0x55), VK_IMAGE_LAYOUT_GENERAL, &ccv, 1, &rng);
		clobber(ccv); clobber(rng);
		vkCmdFillBuffer(cb, hnd<VkBuffer>(0x56), 0, 64, i);
		vkCmdResetQueryPool(cb, hnd<VkQueryPool>(0x60), i, 1);
		vkCmdBeginQuery(cb, hnd<VkQueryPool>(0x60), i, 0);
		vkCmdDraw(cb, 3, 1, i, 0);
		vkCmdEndQuery(cb, hnd<VkQueryPool>(0x60), i);
		vkCmdCopyQueryPoolResults(cb, hnd<VkQueryPool>(0x60), i, 1, hnd<VkBuffer>(0x61), 8ull * i, 8, 0);
		vkCmdDispatch(cb, i, 1, 1);
	}
	if (i % 17 == 0)
	{
		VkImageMemoryBarrier2 ib2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
		ib2.image = hnd<VkImage>(0x800 + i);
		VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &ib2 };
		_vkCmdPipelineBarrier2KHR(cb, &dep);
		clobber(ib2); clobber(dep);
		VkConditionalRenderingBeginInfoEXT cr = { VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT, nullptr, hnd<VkBuffer>(0x70), 4ull * i, 0 };
		_vkCmdBeginConditionalRenderingEXT(cb, &cr);
		clobber(cr);
		VkMultiDrawIndexedInfoEXT md[3] = { { 0, 3u + i, 0 }, { 3, 6, 0 }, { i, 9, 0 } };
		int32_t off = -int32_t(i);
		_vkCmdDrawMultiIndexedEXT(cb, 3, md, 1, 0, sizeof(VkMultiDrawIndexedInfoEXT), &off);
		clobber(md); clobber(off);
		_vkCmdEndConditionalRenderingEXT(cb);
	}
	if (i % 97 == 0)
	{
		// Fallbacks: a pNext chain and a debug label are recorded in place after a drain
		VkDeviceGroupRenderPassBeginInfo chain = { VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO };
		VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, &chain, hnd<VkRenderPass>(0x42), hnd<VkFramebuffer>(0x43), {}, 0, nullptr };
		vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(cb);
		VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, nullptr, "label" };
		_vkCmdInsertDebugUtilsLabelEXT(cb, &label);
	}
	if (i % 89 == 0)
	{
		// A foreign recorder: drains, then records in place
		r2rec::direct_scope scope;
		vkCmdDraw(cb, 1, 2, 3, i);
	}
	if (i % 500 == 499)
	{
		vkEndCommandBuffer(cb);
		g_marks.push_back(log_size());
		vkBeginCommandBuffer(cb, nullptr);
	}
}

static std::vector<std::string> take_log()
{
	std::lock_guard lock(g_log_mtx);
	std::vector<std::string> r;
	r.swap(g_log);
	return r;
}

static void run(int mode, u32 n, bool foreign)
{
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%d", mode);
	_putenv_s("RSX_RECORD_THREAD", buf);
	_putenv_s("RSX_RECORD_RING_MB", "2");
	_putenv_s("RSX_RECORD_STATS", "1");
	r2rec::start();
	take_log();
	g_marks.clear();
	g_off_main = 0;
	g_on_main = 0;

	// mode 1: another thread drains while the producer records; everything published before its drain
	// (the producer's count when the drain started) must be replayed when the drain returns
	std::atomic<bool> stop{false};
	std::atomic<size_t> pushed{0};
	std::atomic<u64> foreign_drains{0}, foreign_bad{0};
	std::thread other;
	if (foreign)
	{
		other = std::thread([&]
		{
			while (!stop.load())
			{
				const size_t before = pushed.load(std::memory_order_acquire);
				r2rec::sync(r2rec::reason::other_thread);
				if (log_size() < before) foreign_bad++;
				foreign_drains++;
			}
		});
	}

	const VkCommandBuffer cb = hnd<VkCommandBuffer>(0xC0);
	vkBeginCommandBuffer(cb, nullptr);
	for (u32 i = 0; i < n; i++)
	{
		emit(i, cb);
		if (mode == 0) g_ref_iter.push_back(log_size());
		// Every call of iterations <= i is published: a drain started from now on must cover them
		if (foreign) pushed.store(g_ref_iter[i], std::memory_order_release);
		if (i % 64 == 0) r2rec::on_flip();
	}
	vkEndCommandBuffer(cb);
	g_marks.push_back(log_size());

	stop = true;
	if (other.joinable()) other.join();
	if (foreign)
	{
		std::printf("  foreign drains %llu\n", static_cast<unsigned long long>(foreign_drains.load()));
		CHECK(foreign_bad == 0, "a foreign drain returned before the published packets were replayed (%llu)", static_cast<unsigned long long>(foreign_bad.load()));
	}
	r2rec::stop();
}

int main()
{
	stubs_init();
	const u32 n = 6000;

	run(0, n, false);
	const std::vector<std::string> ref = take_log();
	g_ref_marks = g_marks;
	std::printf("reference: %zu calls, %zu drains\n", ref.size(), g_ref_marks.size());
	CHECK(g_off_main == 0, "mode 0 recorded off the main thread");

	for (int mode : { 2, 1 })
	{
		std::printf("mode %d\n", mode);
		run(mode, n, mode == 1);
		const std::vector<std::string> got = take_log();
		CHECK(got.size() == ref.size(), "mode %d: %zu calls, reference %zu", mode, got.size(), ref.size());
		size_t bad = 0;
		for (size_t k = 0; k < std::min(got.size(), ref.size()); k++)
		{
			if (got[k] != ref[k] && bad++ < 5)
			{
				std::printf("  #%zu got '%s'\n       ref '%s'\n", k, got[k].c_str(), ref[k].c_str());
			}
		}
		CHECK(bad == 0, "mode %d: %zu calls differ", mode, bad);
		CHECK(g_marks == g_ref_marks, "mode %d: a drain returned before all prior calls were recorded", mode);
		if (mode == 2) CHECK(g_off_main == 0, "mode 2 replayed off the calling thread");
		if (mode == 1) CHECK(g_off_main > ref.size() / 2, "mode 1: only %llu calls on the record thread", static_cast<unsigned long long>(g_off_main.load()));
		std::printf("  on main %llu, on record thread %llu\n", static_cast<unsigned long long>(g_on_main.load()), static_cast<unsigned long long>(g_off_main.load()));
	}

	// Off again after stop(): direct
	vkCmdDraw(hnd<VkCommandBuffer>(0xC1), 1, 1, 1, 1);
	CHECK(log_size() == 1, "after stop() a call was not recorded in place");

	std::printf(g_fail ? "record_thread_test: %d FAILED\n" : "record_thread_test: all passed\n", g_fail);
	return g_fail ? 1 : 0;
}
