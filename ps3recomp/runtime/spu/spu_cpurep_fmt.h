/* cpu-replay: on-disk format of the SPU job capture (PORT_SPU_CAPTURE).
 *
 * Shared by the in-game writer (spu_cpurep.inl, compiled inside spu_drain.c)
 * and the offline replay tool (tools\replay\spu\spu_replay.c). Plain C, no
 * runtime dependencies. See scratch\cpu_replay.md.
 *
 * One file per SPU host thread: <dir>\spu_t<tid>.crsj. All little-endian.
 *   file   := crsj_file_hdr record*
 *   record := u32 type, u32 len, u8 payload[len]
 * Records of one job are contiguous (BEGIN, DMA/ATOM..., END); LS images are
 * stored as 256 x 1 KB chunk ids, each chunk written once per file (the
 * kernel/job code and most data repeat between runs of the same job). */
#ifndef SPU_CPUREP_FMT_H
#define SPU_CPUREP_FMT_H
#include <stdint.h>

#define CRSJ_MAGIC        0x4A535243u   /* "CRSJ" */
#define CRSJ_VERSION      1u
#define CRSJ_CHUNK        1024u
#define CRSJ_LS_SIZE      0x40000u
#define CRSJ_NCHUNK       (CRSJ_LS_SIZE / CRSJ_CHUNK)
#define CRSJ_SLOTS        16u

enum {
    CRSJ_T_CHUNK = 1,   /* u32 id, u8[1024]                                        */
    CRSJ_T_BEGIN = 2,   /* crsj_begin, u8 gpr[2048], u32 chunk_id[256]             */
    CRSJ_T_DMA   = 3,   /* crsj_dma, u8 bytes[size] if flags & (GET|PUT)           */
    CRSJ_T_ATOM  = 4,   /* crsj_atom, u8 ls_line_after[128]                        */
    CRSJ_T_END   = 5,   /* crsj_end, u8 gpr[2048], u32 chunk_id[256] (exit LS)     */
    CRSJ_T_TEXT  = 6    /* free text (session info, profile table)                 */
};

typedef struct crsj_file_hdr {
    uint32_t magic, version;
    uint32_t game;              /* 2 = Resistance 2, 3 = Resistance 3           */
    uint32_t host_tid;
    int32_t  hop_direct, hop_deep, inline_ch;   /* game's dispatch globals      */
    uint32_t ctx_size;          /* sizeof(spu_context) of the capturing build    */
    uint64_t qpf;               /* QueryPerformanceFrequency                     */
    uint32_t reserved[6];
} crsj_file_hdr;

typedef struct crsj_slot { uint32_t lsa, size, source_ea; int32_t image_id, pic; } crsj_slot;

typedef struct crsj_begin {
    uint32_t seq;               /* global capture sequence number              */
    int32_t  frame;             /* g_port_frame_no at job start                */
    int32_t  job_img;           /* image the job timer attributes the run to   */
    uint32_t entry_pc, link;    /* job entry pc, return pc (job ends there)     */
    int32_t  image_id, resident_ovl;
    uint32_t resident_ovl_lsa, ls_bias;
    int32_t  policy_mode, resident_task, module_img_a00;
    uint32_t spu_id, decrementer, event_mask, int_enable, host_depth;
    uint32_t mfc_lsa, mfc_eah, mfc_eal, mfc_size, mfc_tag, mfc_tag_mask, mfc_tag_status;
    uint32_t atomic_stat;
    uint32_t busy_at_start;     /* other jobs running when this one started    */
    uint32_t reserved[3];
    uint64_t qpc;               /* QPC at start (scheduling model)             */
    crsj_slot slot[CRSJ_SLOTS];
} crsj_begin;

#define CRSJ_DMA_GET_BYTES   1u /* bytes = source data the GET delivered        */
#define CRSJ_DMA_PUT_BYTES   2u /* bytes = LS data the PUT sent                  */
#define CRSJ_DMA_LS_WINDOW   4u /* EA was the SPU LS window (bytes read back
                                    from the destination LS after the transfer) */
#define CRSJ_DMA_NO_DATA     8u /* GET source not readable (uncommitted EA)     */
typedef struct crsj_dma {
    uint32_t cmd, lsa, ea_lo, ea_hi, size, flags, pc, reserved;
} crsj_dma;

typedef struct crsj_atom {
    uint32_t cmd, ea, lsa;
    int32_t  rc;                /* spu_mfc_atomic return value                  */
    uint32_t atomic_stat;       /* ctx->atomic_stat after the op                */
    uint32_t pc, reserved[2];
} crsj_atom;

typedef struct crsj_end {
    uint32_t seq;
    uint32_t how;               /* 0 = returned to link, 1 = abandoned/dropped  */
    uint32_t ndma, natom;
    uint32_t exit_pc, reserved;
    uint64_t wall_qpc;          /* game wall time of the run (QPC ticks)        */
    uint64_t cycles;            /* QueryThreadCycleTime delta                   */
} crsj_end;

#endif
