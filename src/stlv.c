/**
 * @file stlv.c
 * @brief sTLV 实现。
 */
#include "stlv.h"

#include <string.h>

/* 帧内偏移 */
#define OFF_SOF0 0u
#define OFF_SOF1 1u
#define OFF_CTL 2u
#define OFF_LEN 3u
#define OFF_HCS 5u
#define OFF_PAYLOAD STLV_HDR_SIZE

/* ------------------------------------------------------------------------ */
/* CRC                                                                       */

#if STLV_CRC_TABLE
static STLV_CONST_ROM const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

uint16_t stlv_crc16(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc = (uint16_t)((crc << 8) ^ crc16_table[(uint8_t)((crc >> 8) ^ *p++)]);
    }
    return crc;
}
#else
uint16_t stlv_crc16(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
#endif

uint8_t stlv_crc8(uint8_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* ------------------------------------------------------------------------ */
/* 字段读写                                                                  */

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr_tlv_hdr(uint8_t *p, stlv_type_t type, stlv_len_t len)
{
#if STLV_TYPE_SIZE == 1
    p[0] = type;
#else
    wr_u16(p, type);
#endif
#if STLV_LEN_SIZE == 1
    p[STLV_TYPE_SIZE] = len;
#else
    wr_u16(p + STLV_TYPE_SIZE, len);
#endif
}

static void rd_tlv_hdr(const uint8_t *p, stlv_type_t *type, stlv_len_t *len)
{
#if STLV_TYPE_SIZE == 1
    *type = p[0];
#else
    *type = stlv_rd_u16(p);
#endif
#if STLV_LEN_SIZE == 1
    *len = p[STLV_TYPE_SIZE];
#else
    *len = stlv_rd_u16(p + STLV_TYPE_SIZE);
#endif
}

/* 写 SOF/CTL/LEN/HCS，返回从 CTL 开始的 CRC 中间值 */
static uint16_t wr_frame_hdr(uint8_t *hdr, uint8_t ctl, uint16_t plen)
{
    hdr[OFF_SOF0] = STLV_SOF0;
    hdr[OFF_SOF1] = STLV_SOF1;
    hdr[OFF_CTL] = ctl;
    wr_u16(hdr + OFF_LEN, plen);
    hdr[OFF_HCS] = stlv_crc8(0, hdr + OFF_CTL, 3);
    return stlv_crc16(0xFFFFu, hdr + OFF_CTL, STLV_HDR_SIZE - OFF_CTL);
}

/* ------------------------------------------------------------------------ */
/* 遍历                                                                      */

void stlv_iter_init(stlv_iter_t *it, const void *payload, size_t len)
{
    it->pos = (const uint8_t *)payload;
    it->end = payload ? it->pos + len : it->pos;
    it->tlv.type = 0;
    it->tlv.len = 0;
    it->tlv.value = NULL;
    it->err = STLV_OK;
}

bool stlv_iter_next(stlv_iter_t *it)
{
    if (it->err != STLV_OK || it->pos == it->end) {
        return false;
    }
    size_t remain = (size_t)(it->end - it->pos);
    if (remain < STLV_TLV_HDR_SIZE) {
        it->err = STLV_ERR_FORMAT;
        return false;
    }
    rd_tlv_hdr(it->pos, &it->tlv.type, &it->tlv.len);
    if (remain - STLV_TLV_HDR_SIZE < it->tlv.len) {
        it->err = STLV_ERR_FORMAT;
        return false;
    }
    it->tlv.value = it->pos + STLV_TLV_HDR_SIZE;
    it->pos += (size_t)STLV_TLV_HDR_SIZE + it->tlv.len;
    return true;
}

int stlv_validate(const void *payload, size_t len)
{
    stlv_iter_t it;
    int count = 0;
    STLV_FOREACH(it, payload, len) {
        count++;
    }
    return it.err != STLV_OK ? it.err : count;
}

/* ------------------------------------------------------------------------ */
/* 封包                                                                      */

int stlv_build_begin(stlv_builder_t *b, void *buf, size_t cap)
{
    if (b == NULL) {
        return STLV_ERR_PARAM;
    }
    b->buf = (uint8_t *)buf;
    b->cap = cap;
    b->len = 0;
    if (buf == NULL || cap < STLV_FRAME_OVERHEAD) {
        b->cap = 0;
        return STLV_ERR_PARAM;
    }
    return STLV_OK;
}

int stlv_build_put(stlv_builder_t *b, stlv_type_t type, const void *value, size_t len)
{
    if (b == NULL || b->cap == 0 || len > STLV_VALUE_MAX || (len != 0 && value == NULL)) {
        return STLV_ERR_PARAM;
    }
    /* 32 位运算，避免 16 位 size_t 平台上回绕 */
    uint32_t total = (uint32_t)b->len + STLV_TLV_HDR_SIZE + (uint32_t)len;
    if (total > STLV_MAX_PAYLOAD || total + STLV_FRAME_OVERHEAD > b->cap) {
        return STLV_ERR_NOSPC;
    }
    uint8_t *p = b->buf + OFF_PAYLOAD + b->len;
    wr_tlv_hdr(p, type, (stlv_len_t)len);
    if (len != 0) {
        memcpy(p + STLV_TLV_HDR_SIZE, value, len);
    }
    b->len = (size_t)total;
    return STLV_OK;
}

int stlv_build_put_items(stlv_builder_t *b, const stlv_tlv_t *items, size_t count)
{
    if (count != 0 && items == NULL) {
        return STLV_ERR_PARAM;
    }
    for (size_t i = 0; i < count; i++) {
        int rc = stlv_build_put(b, items[i].type, items[i].value, items[i].len);
        if (rc != STLV_OK) {
            return rc;
        }
    }
    return STLV_OK;
}

int stlv_build_put_u8(stlv_builder_t *b, stlv_type_t type, uint8_t v)
{
    return stlv_build_put(b, type, &v, 1);
}

int stlv_build_put_u16(stlv_builder_t *b, stlv_type_t type, uint16_t v)
{
    uint8_t d[2];
    wr_u16(d, v);
    return stlv_build_put(b, type, d, sizeof(d));
}

int stlv_build_put_u32(stlv_builder_t *b, stlv_type_t type, uint32_t v)
{
    uint8_t d[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return stlv_build_put(b, type, d, sizeof(d));
}

size_t stlv_build_finish(stlv_builder_t *b, uint8_t ctl)
{
    if (b == NULL || b->cap == 0) {
        return 0;
    }
    uint16_t crc = wr_frame_hdr(b->buf, ctl, (uint16_t)b->len);
    crc = stlv_crc16(crc, b->buf + OFF_PAYLOAD, b->len);
    wr_u16(b->buf + OFF_PAYLOAD + b->len, crc);
    return STLV_FRAME_SIZE(b->len);
}

/* ------------------------------------------------------------------------ */
/* 实例                                                                      */

int stlv_init_abi(stlv_t *ctx, const stlv_cfg_t *cfg, uint32_t abi)
{
    if (abi != STLV_ABI) {
        return STLV_ERR_CONFIG;
    }
    if (ctx == NULL || cfg == NULL || (cfg->types == NULL && cfg->type_count != 0) ||
        (cfg->types != NULL && cfg->type_count == 0)) {
        return STLV_ERR_PARAM;
    }
    for (size_t i = 0; i < cfg->type_count; i++) {
        if (cfg->types[i].min_len > cfg->types[i].max_len) {
            return STLV_ERR_PARAM;
        }
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;
    ctx->tx_syn_left = STLV_SYN_FRAMES;
    return STLV_OK;
}

const stlv_stats_t *stlv_stats(const stlv_t *ctx)
{
    return ctx != NULL ? &ctx->stats : NULL;
}

static void emit(stlv_t *ctx, stlv_event_t ev, const stlv_event_info_t *info)
{
    if (ctx->cfg.on_event != NULL) {
        ctx->cfg.on_event(ctx, ev, info, ctx->cfg.user);
    }
}

static const stlv_type_desc_t *find_type(const stlv_t *ctx, stlv_type_t type)
{
    for (size_t i = 0; i < ctx->cfg.type_count; i++) {
        if (ctx->cfg.types[i].type == type) {
            return &ctx->cfg.types[i];
        }
    }
    return NULL;
}

/*
 * 序号跟踪。规则：
 *  - SYN 帧且不是"紧接上一 SYN 帧的同一启动序列"（上一帧带 SYN 且序号更小）=> 对端重启；
 *  - 否则按 7 位序号差值判断：0 正常，<= STLV_SEQ_GAP_MAX 丢帧，其余为跳变。
 * 首帧只建立基准。先更新状态再发事件，回调中重新 init 也安全。
 */
static void rx_track_seq(stlv_t *ctx, uint8_t ctl, stlv_event_info_t *info)
{
    uint8_t seq = (uint8_t)(ctl & STLV_SEQ_MASK);
    bool syn = (ctl & STLV_FLAG_SYN) != 0;
    bool known = ctx->rx_seq_valid != 0;
    uint8_t prev = (uint8_t)((ctx->rx_expect_seq - 1u) & STLV_SEQ_MASK);
    bool same_burst = ctx->rx_last_syn && seq > prev;
    uint8_t gap = (uint8_t)((seq - ctx->rx_expect_seq) & STLV_SEQ_MASK);

    ctx->rx_expect_seq = (uint8_t)((seq + 1u) & STLV_SEQ_MASK);
    ctx->rx_seq_valid = 1;
    ctx->rx_last_syn = syn;

    if (!known) {
        return;
    }
    if (syn && !same_burst) {
        ctx->stats.rx_peer_reset++;
        emit(ctx, STLV_EV_PEER_RESET, info);
    } else if (gap == 0) {
        /* 连续 */
    } else if (gap <= STLV_SEQ_GAP_MAX) {
        info->lost = gap;
        ctx->stats.rx_lost += gap;
        emit(ctx, STLV_EV_SEQ_GAP, info);
        info->lost = 0;
    } else {
        ctx->stats.rx_seq_jump++;
        emit(ctx, STLV_EV_SEQ_JUMP, info);
    }
}

/* 分发一个 CRC 正确的帧；TLV 结构损坏时返回 false（多半是误判的假帧，由调用方重扫） */
static bool rx_deliver(stlv_t *ctx, uint8_t ctl, const uint8_t *payload, uint16_t plen)
{
    stlv_event_info_t info = {(uint8_t)(ctl & STLV_SEQ_MASK), 0, plen, NULL};

    if (stlv_validate(payload, plen) < 0) {
        ctx->stats.err_format++;
        emit(ctx, STLV_EV_ERR_FORMAT, &info);
        return false;
    }

    ctx->stats.rx_frames++;
    rx_track_seq(ctx, ctl, &info);
    if (!ctx->rx_busy) {
        return true;
    }
    emit(ctx, STLV_EV_FRAME_BEGIN, &info);

    /* 每次回调后检查 rx_busy：回调中重新 init 会清零它，此时 payload 已失效 */
    stlv_iter_t it;
    STLV_FOREACH(it, payload, plen) {
        if (!ctx->rx_busy) {
            return true;
        }
        if (ctx->cfg.types != NULL) {
            const stlv_type_desc_t *d = find_type(ctx, it.tlv.type);
            info.tlv = &it.tlv;
            if (d == NULL) {
                ctx->stats.tlv_unknown++;
                emit(ctx, STLV_EV_TLV_UNKNOWN, &info);
                continue;
            }
            if (it.tlv.len < d->min_len || it.tlv.len > d->max_len) {
                ctx->stats.tlv_bad_len++;
                emit(ctx, STLV_EV_TLV_BAD_LEN, &info);
                continue;
            }
        }
        ctx->stats.rx_tlvs++;
        if (ctx->cfg.on_tlv != NULL) {
            ctx->cfg.on_tlv(ctx, &it.tlv, ctx->cfg.user);
        }
    }

    if (ctx->rx_busy) {
        info.tlv = NULL;
        emit(ctx, STLV_EV_FRAME_END, &info);
    }
    return true;
}

/* 从缓冲区头部移除 n 字节（O(1)，只移动读指针）；discard 为真表示这些字节不属于有效帧 */
static void rx_drop(stlv_t *ctx, size_t n, bool discard)
{
    if (n > ctx->rx_len) {
        n = ctx->rx_len; /* 回调中重新 init 后缓冲区已清空 */
    }
    if (discard) {
        ctx->stats.rx_discarded += (uint32_t)n;
    }
    ctx->rx_len -= (uint32_t)n;
    ctx->rx_head = ctx->rx_len != 0 ? ctx->rx_head + (uint32_t)n : 0;
}

/*
 * 解析缓冲区中所有能确定结果的数据。任何校验失败（含 TLV 结构损坏）只丢弃 1 字节再重新寻找 SOF，
 * 因此被损坏帧"吞进来"的后续完整帧不会丢失。返回时缓冲区要么为空，
 * 要么头部是一个尚未收齐的候选帧，且 rx_len < sizeof(rx_buf)。
 */
static void rx_process(stlv_t *ctx)
{
    while (ctx->rx_len != 0 && ctx->rx_busy) {
        const uint8_t *b = ctx->rx_buf + ctx->rx_head;
        size_t n = ctx->rx_len;

        if (b[OFF_SOF0] != STLV_SOF0) {
            const uint8_t *p = (const uint8_t *)memchr(b + 1, STLV_SOF0, n - 1);
            rx_drop(ctx, p != NULL ? (size_t)(p - b) : n, true);
            continue;
        }
        if (n < 2) {
            return;
        }
        if (b[OFF_SOF1] != STLV_SOF1) {
            rx_drop(ctx, 1, true);
            continue;
        }
        if (n < STLV_HDR_SIZE) {
            return;
        }

        uint16_t plen = stlv_rd_u16(b + OFF_LEN);
        if (stlv_crc8(0, b + OFF_CTL, 3) != b[OFF_HCS]
#if STLV_MAX_PAYLOAD < 65535
            || plen > STLV_MAX_PAYLOAD
#endif
        ) {
            stlv_event_info_t info = {b[OFF_CTL], 0, plen, NULL};
            ctx->stats.err_header++;
            emit(ctx, STLV_EV_ERR_HEADER, &info);
            rx_drop(ctx, 1, true);
            continue;
        }

        size_t flen = STLV_FRAME_SIZE(plen);
        if (n < flen) {
            return;
        }

        uint16_t crc = stlv_crc16(0xFFFFu, b + OFF_CTL, flen - OFF_CTL - STLV_CRC_SIZE);
        if (crc != stlv_rd_u16(b + OFF_PAYLOAD + plen)) {
            stlv_event_info_t info = {b[OFF_CTL], 0, plen, NULL};
            ctx->stats.err_crc++;
            emit(ctx, STLV_EV_ERR_CRC, &info);
            rx_drop(ctx, 1, true);
            continue;
        }

        if (rx_deliver(ctx, b[OFF_CTL], b + OFF_PAYLOAD, plen)) {
            rx_drop(ctx, flen, false);
        } else {
            rx_drop(ctx, 1, true);
        }
    }
}

int stlv_rx_feed(stlv_t *ctx, const void *data, size_t len)
{
    if (ctx == NULL || (len != 0 && data == NULL)) {
        return STLV_ERR_PARAM;
    }
    if (ctx->rx_busy) {
        return STLV_ERR_BUSY;
    }
    ctx->rx_busy = 1;

    const uint8_t *p = (const uint8_t *)data;
    while (len != 0) {
        if (ctx->rx_head + ctx->rx_len == sizeof(ctx->rx_buf)) {
            /* 尾部已满才整理一次，摊还 O(1) */
            memmove(ctx->rx_buf, ctx->rx_buf + ctx->rx_head, ctx->rx_len);
            ctx->rx_head = 0;
        }
        size_t tail = ctx->rx_head + ctx->rx_len;
        size_t room = sizeof(ctx->rx_buf) - tail;
        size_t k = len < room ? len : room;
        memcpy(ctx->rx_buf + tail, p, k);
        ctx->rx_len += (uint32_t)k;
        p += k;
        len -= k;
        rx_process(ctx);
        ctx->rx_busy = 1; /* 回调中可能重新 init，剩余数据交给新状态 */
    }

    ctx->rx_busy = 0;
    return STLV_OK;
}

int stlv_rx_flush(stlv_t *ctx)
{
    if (ctx == NULL) {
        return STLV_ERR_PARAM;
    }
    if (ctx->rx_busy) {
        return STLV_ERR_BUSY;
    }
    ctx->rx_busy = 1;
    while (ctx->rx_len != 0) {
        rx_drop(ctx, 1, true);
        rx_process(ctx);
        ctx->rx_busy = 1;
    }
    ctx->rx_busy = 0;
    return STLV_OK;
}

/* ------------------------------------------------------------------------ */
/* 发送                                                                      */

static int tx_write(stlv_t *ctx, const uint8_t *data, size_t len)
{
    return ctx->cfg.write(data, len, ctx->cfg.user) == 0 ? STLV_OK : STLV_ERR_IO;
}

/* 取下一帧的 CTL 并推进发送状态（写失败也推进，接收端据此识别丢帧） */
static uint8_t tx_next_ctl(stlv_t *ctx)
{
    uint8_t ctl = ctx->tx_seq;
    if (ctx->tx_syn_left != 0) {
        ctl |= STLV_FLAG_SYN;
        ctx->tx_syn_left--;
    }
    ctx->tx_seq = (uint8_t)((ctx->tx_seq + 1u) & STLV_SEQ_MASK);
    return ctl;
}

int stlv_send(stlv_t *ctx, stlv_builder_t *b)
{
    if (ctx == NULL || ctx->cfg.write == NULL || b == NULL || b->cap == 0) {
        return STLV_ERR_PARAM;
    }
    size_t flen = stlv_build_finish(b, tx_next_ctl(ctx));
    int rc = tx_write(ctx, b->buf, flen);
    if (rc == STLV_OK) {
        ctx->stats.tx_frames++;
    }
    return rc;
}

int stlv_send_tlv(stlv_t *ctx, stlv_type_t type, const void *value, size_t len)
{
    if (ctx == NULL || ctx->cfg.write == NULL || len > STLV_VALUE_MAX ||
        (len != 0 && value == NULL)) {
        return STLV_ERR_PARAM;
    }
    uint32_t plen = (uint32_t)STLV_TLV_HDR_SIZE + (uint32_t)len;
    if (plen > STLV_MAX_PAYLOAD) {
        return STLV_ERR_NOSPC;
    }

    uint8_t hdr[STLV_HDR_SIZE + STLV_TLV_HDR_SIZE];
    uint16_t crc = wr_frame_hdr(hdr, tx_next_ctl(ctx), (uint16_t)plen);
    wr_tlv_hdr(hdr + STLV_HDR_SIZE, type, (stlv_len_t)len);
    crc = stlv_crc16(crc, hdr + STLV_HDR_SIZE, STLV_TLV_HDR_SIZE);
    crc = stlv_crc16(crc, value, len);
    uint8_t tail[STLV_CRC_SIZE];
    wr_u16(tail, crc);

    int rc = tx_write(ctx, hdr, sizeof(hdr));
    if (rc == STLV_OK && len != 0) {
        rc = tx_write(ctx, (const uint8_t *)value, len);
    }
    if (rc == STLV_OK) {
        rc = tx_write(ctx, tail, sizeof(tail));
    }
    if (rc == STLV_OK) {
        ctx->stats.tx_frames++;
    }
    return rc;
}
