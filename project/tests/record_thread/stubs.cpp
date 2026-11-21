// record_thread_test: the "driver". Every Vulkan entry point the record ring can replay, logged in call order.
#define R2REC_NO_REDIRECT
#include "stdafx.h"
#include "Emu/RSX/VK/VulkanAPI.h"
#define DECLARE_VK_FUNCTION_BODY
#include "Emu/RSX/VK/VKProcTable.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

std::mutex g_log_mtx;
std::vector<std::string> g_log;
std::thread::id g_main_id;
std::atomic<u64> g_off_main{0}, g_on_main{0};

static void L(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	(std::this_thread::get_id() == g_main_id ? g_on_main : g_off_main)++;
	std::lock_guard lock(g_log_mtx);
	g_log.emplace_back(buf);
}

static unsigned long long H(const void* h) { return reinterpret_cast<unsigned long long>(h); }
template <typename T> static unsigned long long H(T h) { return (unsigned long long)(h); }

static u32 sum_bytes(const void* p, size_t n)
{
	u32 s = 2166136261u;
	for (size_t i = 0; i < n; i++) s = (s ^ static_cast<const u8*>(p)[i]) * 16777619u;
	return s;
}

void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline p) { L("pipe %llx %d %llx", H(c), bp, H(p)); }
void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c, uint32_t f, uint32_t n, const VkViewport* v) { L("vp %u %u %g %g %g %g", f, n, v[0].x, v[0].y, v[n - 1].width, v[n - 1].maxDepth); }
void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c, uint32_t f, uint32_t n, const VkRect2D* r) { L("sc %u %d %u", n, r[0].offset.x, r[n - 1].extent.width); }
void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer, float w) { L("lw %g", w); }
void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer, float a, float b, float s) { L("db %g %g %g", a, b, s); }
void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer, const float k[4]) { L("bc %g %g %g %g", k[0], k[1], k[2], k[3]); }
void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer, float a, float b) { L("dbo %g %g", a, b); }
void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer, VkStencilFaceFlags f, uint32_t m) { L("scm %u %u", f, m); }
void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer, VkStencilFaceFlags f, uint32_t m) { L("swm %u %u", f, m); }
void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer, VkStencilFaceFlags f, uint32_t m) { L("sref %u %u", f, m); }
void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout l, uint32_t f, uint32_t n, const VkDescriptorSet* s, uint32_t dn, const uint32_t* d)
{
	L("bds %llx %u %u %llx %llx %u %u", H(l), f, n, H(s[0]), H(s[n - 1]), dn, dn ? d[dn - 1] : 0u);
}
void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkIndexType t) { L("ib %llx %llu %d", H(b), o, t); }
void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer, uint32_t f, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) { L("vb %u %u %llx %llu", f, n, H(b[n - 1]), o[n - 1]); }
void VKAPI_CALL vkCmdDraw(VkCommandBuffer c, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) { L("draw %llx %u %u %u %u", H(c), vc, ic, fv, fi); }
void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer c, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi) { L("drawi %llx %u %u %u %d %u", H(c), xc, ic, fx, vo, fi); }
void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t x, uint32_t y, uint32_t z) { L("disp %u %u %u", x, y, z); }
void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) { L("cpb %llx %llx %u %llu", H(s), H(d), n, r[n - 1].size); }
void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer, VkImage s, VkImageLayout, VkImage d, VkImageLayout, uint32_t n, const VkImageCopy* r) { L("cpi %llx %llx %u %u", H(s), H(d), n, r[n - 1].extent.width); }
void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer, VkImage s, VkImageLayout, VkImage d, VkImageLayout, uint32_t n, const VkImageBlit* r, VkFilter f) { L("blit %llx %llx %u %d %d", H(s), H(d), n, r[0].dstOffsets[1].x, f); }
void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer s, VkImage d, VkImageLayout, uint32_t n, const VkBufferImageCopy* r) { L("b2i %llx %llx %u %llu", H(s), H(d), n, r[n - 1].bufferOffset); }
void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage s, VkImageLayout, VkBuffer d, uint32_t n, const VkBufferImageCopy* r) { L("i2b %llx %llx %u %llu", H(s), H(d), n, r[n - 1].bufferOffset); }
void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, const void* p) { L("ub %llx %llu %llu %08x", H(b), o, sz, sum_bytes(p, static_cast<size_t>(sz))); }
void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, uint32_t v) { L("fb %llx %llu %llu %u", H(b), o, sz, v); }
void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer, VkImage i, VkImageLayout, const VkClearColorValue* v, uint32_t n, const VkImageSubresourceRange* r) { L("cci %llx %g %u %u", H(i), v->float32[3], n, r[n - 1].levelCount); }
void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer, VkImage i, VkImageLayout, const VkClearDepthStencilValue* v, uint32_t n, const VkImageSubresourceRange*) { L("cdi %llx %g %u", H(i), v->depth, n); }
void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer, uint32_t an, const VkClearAttachment* a, uint32_t rn, const VkClearRect* r) { L("cla %u %u %u %u", an, a[an - 1].colorAttachment, rn, r[rn - 1].rect.extent.height); }
void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer, VkEvent e, VkPipelineStageFlags m) { L("sev %llx %u", H(e), m); }
void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer, uint32_t en, const VkEvent* e, VkPipelineStageFlags, VkPipelineStageFlags, uint32_t mn, const VkMemoryBarrier*, uint32_t bn, const VkBufferMemoryBarrier*, uint32_t in, const VkImageMemoryBarrier* i)
{
	L("wev %u %llx %u %u %u %llx", en, H(e[en - 1]), mn, bn, in, in ? H(i[in - 1].image) : 0ull);
}
void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags, uint32_t mn, const VkMemoryBarrier* m, uint32_t bn, const VkBufferMemoryBarrier* b, uint32_t in, const VkImageMemoryBarrier* i)
{
	L("bar %u %u %u %u %u %u %llx %u", s, d, mn, mn ? m[0].srcAccessMask : 0u, bn, in, in ? H(i[in - 1].image) : 0ull, in ? i[in - 1].newLayout : 0);
}
void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer, VkQueryPool p, uint32_t q, VkQueryControlFlags f) { L("bq %llx %u %u", H(p), q, f); }
void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer, VkQueryPool p, uint32_t q) { L("eq %llx %u", H(p), q); }
void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool p, uint32_t f, uint32_t n) { L("rqp %llx %u %u", H(p), f, n); }
void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t q) { L("ts %d %llx %u", s, H(p), q); }
void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool p, uint32_t f, uint32_t n, VkBuffer b, VkDeviceSize o, VkDeviceSize st, VkQueryResultFlags fl)
{
	L("cqp %llx %u %u %llx %llu %llu %u", H(p), f, n, H(b), o, st, fl);
}
void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout l, VkShaderStageFlags s, uint32_t o, uint32_t sz, const void* v) { L("pc %llx %u %u %u %08x", H(l), s, o, sz, sum_bytes(v, sz)); }
void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo* i, VkSubpassContents s)
{
	L("brp %llx %llx %u %g %d", H(i->renderPass), H(i->framebuffer), i->clearValueCount, i->clearValueCount ? i->pClearValues[i->clearValueCount - 1].color.float32[0] : 0.f, s);
}
void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer) { L("erp"); }

static void log_writes(const char* tag, uint32_t n, const VkWriteDescriptorSet* w)
{
	std::string s = tag;
	char buf[128];
	for (uint32_t i = 0; i < n; i++)
	{
		std::snprintf(buf, sizeof(buf), " [%llx %u %u %d", H(w[i].dstSet), w[i].dstBinding, w[i].descriptorCount, w[i].descriptorType);
		s += buf;
		for (uint32_t k = 0; k < w[i].descriptorCount; k++)
		{
			if (w[i].pImageInfo) std::snprintf(buf, sizeof(buf), " i%llx/%llx/%d", H(w[i].pImageInfo[k].sampler), H(w[i].pImageInfo[k].imageView), w[i].pImageInfo[k].imageLayout);
			else if (w[i].pBufferInfo) std::snprintf(buf, sizeof(buf), " b%llx/%llu/%llu", H(w[i].pBufferInfo[k].buffer), w[i].pBufferInfo[k].offset, w[i].pBufferInfo[k].range);
			else std::snprintf(buf, sizeof(buf), " t%llx", H(w[i].pTexelBufferView[k]));
			s += buf;
		}
		s += "]";
	}
	L("%s", s.c_str());
}
void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t wn, const VkWriteDescriptorSet* w, uint32_t cn, const VkCopyDescriptorSet*)
{
	log_writes(cn ? "uds+copy" : "uds", wn, w);
}
static void VKAPI_CALL stub_push_desc(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout l, uint32_t s, uint32_t n, const VkWriteDescriptorSet* w) { log_writes("push", n, w); }
static void VKAPI_CALL stub_begin_cond(VkCommandBuffer, const VkConditionalRenderingBeginInfoEXT* i) { L("bcond %llx %llu %u", H(i->buffer), i->offset, i->flags); }
static void VKAPI_CALL stub_end_cond(VkCommandBuffer) { L("econd"); }
static void VKAPI_CALL stub_label(VkCommandBuffer, const VkDebugUtilsLabelEXT* l) { L("label %s", l->pLabelName); }
static void VKAPI_CALL stub_bar2(VkCommandBuffer, const VkDependencyInfo* d)
{
	L("bar2 %u %u %u %llx", d->memoryBarrierCount, d->bufferMemoryBarrierCount, d->imageMemoryBarrierCount, d->imageMemoryBarrierCount ? H(d->pImageMemoryBarriers[0].image) : 0ull);
}
static void VKAPI_CALL stub_sev2(VkCommandBuffer, VkEvent e, const VkDependencyInfo* d) { L("sev2 %llx %u", H(e), d->imageMemoryBarrierCount); }
static void VKAPI_CALL stub_wev2(VkCommandBuffer, uint32_t n, const VkEvent* e, const VkDependencyInfo* d) { L("wev2 %u %llx %u", n, H(e[n - 1]), d[n - 1].bufferMemoryBarrierCount); }
static void VKAPI_CALL stub_multi(VkCommandBuffer, uint32_t n, const VkMultiDrawInfoEXT* d, uint32_t ic, uint32_t fi, uint32_t st)
{
	const auto* last = reinterpret_cast<const VkMultiDrawInfoEXT*>(reinterpret_cast<const u8*>(d) + static_cast<size_t>(n - 1) * st);
	L("multi %u %u %u %u", n, d[0].vertexCount, last->firstVertex, ic);
}
static void VKAPI_CALL stub_multi_i(VkCommandBuffer, uint32_t n, const VkMultiDrawIndexedInfoEXT* d, uint32_t ic, uint32_t fi, uint32_t st, const int32_t* vo)
{
	const auto* last = reinterpret_cast<const VkMultiDrawIndexedInfoEXT*>(reinterpret_cast<const u8*>(d) + static_cast<size_t>(n - 1) * st);
	L("multii %u %u %u %d", n, d[0].indexCount, last->firstIndex, vo ? *vo : -999);
}

VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo*) { L("begin %llx", H(c)); return VK_SUCCESS; }
VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c) { L("end %llx", H(c)); return VK_SUCCESS; }
VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { L("submit"); return VK_SUCCESS; }
void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) { L("destroy %llx", H(b)); }

void stubs_init()
{
	g_main_id = std::this_thread::get_id();
	_vkCmdPushDescriptorSetKHR = stub_push_desc;
	_vkCmdBeginConditionalRenderingEXT = stub_begin_cond;
	_vkCmdEndConditionalRenderingEXT = stub_end_cond;
	_vkCmdInsertDebugUtilsLabelEXT = stub_label;
	_vkCmdPipelineBarrier2KHR = stub_bar2;
	_vkCmdSetEvent2KHR = stub_sev2;
	_vkCmdWaitEvents2KHR = stub_wev2;
	_vkCmdDrawMultiEXT = stub_multi;
	_vkCmdDrawMultiIndexedEXT = stub_multi_i;
}
