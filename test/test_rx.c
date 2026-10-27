/* 实例收发、重同步、类型表、回调行为测试 */
#include <string.h>

#include "stlv.h"
#include "tinytest.h"

#define MAX_REC 64
#define TX_CAP (4 * STLV_MAX_FRAME > 2048 ? 4 * STLV_MAX_FRAME : 2048)

typedef struct {
    /* 收到的 TLV */
    int n_tlv;
    stlv_type_t type[MAX_REC];
    stlv_len_t len[MAX_REC];
    uint8_t first[MAX_REC];
    /* 事件 */
    int ev_count[STLV_EV_PEER_RESET + 1];
    uint8_t last_seq;
    uint8_t last_lost;
    /* 发送捕获 */
    uint8_t tx[TX_CAP];
    size_t tx_len;
    int tx_calls;
    int tx_fail;
    /* 回调内动作 */
    stlv_t *peer; /* 收到 TLV 后回显到 peer */
    int reenter_rc;
    int flush_rc;
    int reinit_at;      /* 第 n 个 TLV（从 1 计）时在回调里重新 init */
    int reinit_on_begin; /* 收到 FRAME_BEGIN 时重新 init */
    int fail_write_at;  /* 第 n 次 write 调用失败（从 1 计） */
} cap_t;

static void on_tlv(stlv_t *ctx, const stlv_tlv_t *tlv, void *user);
static void on_event(stlv_t *ctx, stlv_event_t ev, const stlv_event_info_t *info, void *user);
static int cap_write(const uint8_t *data, size_t len, void *user);

static void reinit(stlv_t *s, cap_t *c)
{
    stlv_cfg_t cfg = {cap_write, on_tlv, on_event, NULL, 0, c};
    CHECK_EQ(stlv_init(s, &cfg), STLV_OK);
}

static void on_tlv(stlv_t *ctx, const stlv_tlv_t *tlv, void *user)
{
    cap_t *c = (cap_t *)user;
    if (c->n_tlv < MAX_REC) {
        c->type[c->n_tlv] = tlv->type;
        c->len[c->n_tlv] = tlv->len;
        c->first[c->n_tlv] = tlv->len ? tlv->value[0] : 0;
    }
    c->n_tlv++;
    if (c->reinit_at == c->n_tlv) {
        reinit(ctx, c);
        return;
    }
    c->reenter_rc = stlv_rx_feed(ctx, "x", 1);
    c->flush_rc = stlv_rx_flush(ctx);
    if (c->peer != NULL) {
        stlv_send_tlv(c->peer, tlv->type, tlv->value, tlv->len);
    }
}

static void on_event(stlv_t *ctx, stlv_event_t ev, const stlv_event_info_t *info, void *user)
{
    (void)ctx;
    cap_t *c = (cap_t *)user;
    c->ev_count[ev]++;
    c->last_seq = info->seq;
    if (ev == STLV_EV_SEQ_GAP) {
        c->last_lost = info->lost;
    }
    if (ev == STLV_EV_FRAME_BEGIN && c->reinit_on_begin) {
        c->reinit_on_begin = 0;
        reinit(ctx, c);
    }
}

static int cap_write(const uint8_t *data, size_t len, void *user)
{
    cap_t *c = (cap_t *)user;
    c->tx_calls++;
    if (c->tx_fail || c->fail_write_at == c->tx_calls) {
        return -1;
    }
    CHECK(c->tx_len + len <= sizeof(c->tx));
    if (c->tx_len + len <= sizeof(c->tx)) {
        memcpy(c->tx + c->tx_len, data, len);
        c->tx_len += len;
    }
    return 0;
}

static void setup(stlv_t *s, cap_t *c, const stlv_type_desc_t *types, size_t n)
{
    memset(c, 0, sizeof(*c));
    stlv_cfg_t cfg = {cap_write, on_tlv, on_event, types, n, c};
    CHECK_EQ(stlv_init(s, &cfg), STLV_OK);
}

/* 用实例发送一帧并取回字节 */
static size_t make_frame(uint8_t *out, uint8_t seq, const stlv_tlv_t *items, size_t n)
{
    stlv_builder_t b;
    stlv_build_begin(&b, out, STLV_MAX_FRAME);
    CHECK_EQ(stlv_build_put_items(&b, items, n), STLV_OK);
    return stlv_build_finish(&b, seq);
}

static const uint8_t V[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
static const stlv_tlv_t ITEMS[] = {{1, 2, V}, {2, 0, NULL}, {3, 8, V}};

static void test_init_params(void)
{
    stlv_t s;
    stlv_cfg_t cfg = {0};
    CHECK_EQ(stlv_init(NULL, &cfg), STLV_ERR_PARAM);
    CHECK_EQ(stlv_init_abi(&s, &cfg, STLV_ABI ^ 1u), STLV_ERR_CONFIG);
    CHECK(stlv_stats(NULL) == NULL);
    CHECK_EQ(stlv_init(&s, NULL), STLV_ERR_PARAM);
    CHECK_EQ(stlv_init(&s, &cfg), STLV_OK);
    cfg.type_count = 1;
    CHECK_EQ(stlv_init(&s, &cfg), STLV_ERR_PARAM);
    const stlv_type_desc_t bad[] = {{1, 4, 2}};
    cfg.types = bad;
    CHECK_EQ(stlv_init(&s, &cfg), STLV_ERR_PARAM);
    cfg.type_count = 0;
    CHECK_EQ(stlv_init(&s, &cfg), STLV_ERR_PARAM);
    /* 未配置 write 时不能发送 */
    cfg.types = NULL;
    CHECK_EQ(stlv_init(&s, &cfg), STLV_OK);
    CHECK_EQ(stlv_send_tlv(&s, 1, NULL, 0), STLV_ERR_PARAM);
}

static void test_feed_whole_and_bytewise(void)
{
    stlv_t s;
    cap_t c;
    uint8_t f[STLV_MAX_FRAME];
    size_t n = make_frame(f, 7, ITEMS, 3);

    setup(&s, &c, NULL, 0);
    CHECK_EQ(stlv_rx_feed(&s, f, n), STLV_OK);
    CHECK_EQ(c.n_tlv, 3);
    CHECK_EQ(c.type[0], 1);
    CHECK_EQ(c.len[0], 2);
    CHECK_EQ(c.first[0], 0x11);
    CHECK_EQ(c.type[1], 2);
    CHECK_EQ(c.len[1], 0);
    CHECK_EQ(c.len[2], 8);
    CHECK_EQ(c.ev_count[STLV_EV_FRAME_BEGIN], 1);
    CHECK_EQ(c.ev_count[STLV_EV_FRAME_END], 1);
    CHECK_EQ(c.last_seq, 7);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 1);
    CHECK_EQ(stlv_stats(&s)->rx_tlvs, 3);
    CHECK_EQ(stlv_stats(&s)->rx_discarded, 0);
    /* 回调内重入被拒绝 */
    CHECK_EQ(c.reenter_rc, STLV_ERR_BUSY);
    CHECK_EQ(c.flush_rc, STLV_ERR_BUSY);

    setup(&s, &c, NULL, 0);
    for (size_t i = 0; i < n; i++) {
        stlv_rx_feed(&s, f + i, 1);
        CHECK_EQ(c.n_tlv, i + 1 == n ? 3 : 0);
    }
}

static void test_multi_frames_and_garbage(void)
{
    stlv_t s;
    cap_t c;
    uint8_t stream[4 * STLV_MAX_FRAME];
    size_t n = 0;

    /* 噪声中混入 SOF 字节和假帧头 */
    static const uint8_t junk[] = {0x00, 0xA5, 0xA5, 0x5A, 0x01, 0xFF, 0xFF, 0x00, 0x5A, 0xA5};
    memcpy(stream + n, junk, sizeof(junk));
    n += sizeof(junk);
    n += make_frame(stream + n, 0, ITEMS, 1);
    stream[n++] = 0xA5; /* 两帧之间的孤立 SOF0 */
    n += make_frame(stream + n, 1, ITEMS + 1, 2);
    n += make_frame(stream + n, 2, NULL, 0); /* 空 payload 帧 */

    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, stream, n);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 3);
    CHECK_EQ(c.n_tlv, 3);
    CHECK_EQ(c.ev_count[STLV_EV_SEQ_GAP], 0);
    CHECK_EQ(stlv_stats(&s)->rx_discarded, sizeof(junk) + 1);
}

static void test_payload_contains_sof(void)
{
    static const uint8_t evil[] = {0xA5, 0x5A, 0xA5, 0x5A, 0x00, 0x03, 0x00, 0xA5};
    const stlv_tlv_t it[] = {{0xA5, sizeof(evil), evil}};
    uint8_t f[STLV_MAX_FRAME];
    size_t n = make_frame(f, 0xA5, it, 1);

    stlv_t s;
    cap_t c;
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, f, n);
    CHECK_EQ(c.n_tlv, 1);
    CHECK_EQ(c.len[0], sizeof(evil));
    CHECK_EQ(stlv_stats(&s)->rx_discarded, 0);
}

static void test_bad_frames_recover(void)
{
    uint8_t a[STLV_MAX_FRAME], b[STLV_MAX_FRAME];
    size_t na = make_frame(a, 0, ITEMS, 3);
    size_t nb = make_frame(b, 1, ITEMS, 1);
    uint8_t stream[3 * STLV_MAX_FRAME];
    stlv_t s;
    cap_t c;

    /* CRC 错误 */
    memcpy(stream, a, na);
    stream[na - 3] ^= 0x01;
    memcpy(stream + na, b, nb);
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, stream, na + nb);
    CHECK_EQ(stlv_stats(&s)->err_crc, 1);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 1);
    CHECK_EQ(c.n_tlv, 1);
    CHECK_EQ(c.ev_count[STLV_EV_ERR_CRC], 1);
    CHECK_EQ(stlv_stats(&s)->rx_discarded, na);

    /* 帧头 HCS 错误 */
    memcpy(stream, a, na);
    stream[2] ^= 0x80;
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, stream, na + nb);
    CHECK_EQ(stlv_stats(&s)->err_header, 1);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 1);

    /* LEN 超过上限（HCS 正确） */
    uint8_t h[STLV_HDR_SIZE] = {STLV_SOF0, STLV_SOF1, 0, 0, 0, 0};
    uint16_t big = (uint16_t)(STLV_MAX_PAYLOAD + 1u);
    h[3] = (uint8_t)big;
    h[4] = (uint8_t)(big >> 8);
    h[5] = stlv_crc8(0, h + 2, 3);
    if (STLV_MAX_PAYLOAD < 65535) {
        memcpy(stream, h, sizeof(h));
        memcpy(stream + sizeof(h), b, nb);
        setup(&s, &c, NULL, 0);
        stlv_rx_feed(&s, stream, sizeof(h) + nb);
        CHECK_EQ(stlv_stats(&s)->err_header, 1);
        CHECK_EQ(stlv_stats(&s)->rx_frames, 1);
    }

    /* 丢一个字节：坏帧吞掉下一帧的开头，但下一帧仍能恢复 */
    memcpy(stream, a, na);
    memmove(stream + 8, stream + 9, na - 9); /* 删除 payload 中一个字节 */
    memcpy(stream + na - 1, b, nb);
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, stream, na - 1 + nb);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 1);
    CHECK_EQ(c.last_seq, 1);
    CHECK_EQ(stlv_stats(&s)->err_crc, 1);
}

static void test_flush_releases_waiting_frame(void)
{
    uint8_t a[STLV_MAX_FRAME], b[STLV_MAX_FRAME];
    size_t na = make_frame(a, 0, ITEMS, 3);
    size_t nb = make_frame(b, 1, ITEMS, 1);
    uint8_t stream[2 * STLV_MAX_FRAME];
    stlv_t s;
    cap_t c;

    /* a 被截断到只剩帧头+少量 payload，后面紧跟完整的 b；b 比 a 短，
       解析器仍在等待 a 收齐，直到 flush 才释放 b */
    size_t cut = STLV_HDR_SIZE + 2;
    memcpy(stream, a, cut);
    memcpy(stream + cut, b, nb);
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, stream, cut + nb);
    if (cut + nb < na) {
        CHECK_EQ(stlv_stats(&s)->rx_frames, 0);
    }
    CHECK_EQ(stlv_rx_flush(&s), STLV_OK);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 1);
    CHECK_EQ(c.last_seq, 1);
    CHECK_EQ(stlv_stats(&s)->rx_discarded, cut);
    CHECK_EQ(stlv_rx_flush(&s), STLV_OK); /* 空缓冲区 flush 无副作用 */
    CHECK_EQ(stlv_stats(&s)->rx_discarded, cut);
}

static void test_seq_tracking(void)
{
    uint8_t f[STLV_MAX_FRAME];
    stlv_t s;
    cap_t c;
    setup(&s, &c, NULL, 0);

    /* 首帧任意序号，不报丢帧；126 -> 127 -> 0 回绕连续；1 -> 5 丢 3 帧；
       6 -> 6 重复帧、6 -> 3 回退、4 -> 5+GAP_MAX+1 前跳过大都视为跳变；
       最后一帧前跳恰好 STLV_SEQ_GAP_MAX，仍算丢帧 */
    const uint8_t j = (uint8_t)((5 + STLV_SEQ_GAP_MAX + 1) & STLV_SEQ_MASK);
    const uint8_t seqs[] = {126, 127, 0, 1, 5, 6, 6, 3, 4, j,
                            (uint8_t)((j + 1 + STLV_SEQ_GAP_MAX) & STLV_SEQ_MASK)};
    for (size_t i = 0; i < sizeof(seqs); i++) {
        size_t n = make_frame(f, seqs[i], ITEMS, 1);
        stlv_rx_feed(&s, f, n);
        if (i == 4) {
            CHECK_EQ(c.ev_count[STLV_EV_SEQ_GAP], 1);
            CHECK_EQ(c.last_lost, 3);
        }
    }
    CHECK_EQ(stlv_stats(&s)->rx_frames, sizeof(seqs));
    CHECK_EQ(c.ev_count[STLV_EV_SEQ_JUMP], 3);
    CHECK_EQ(stlv_stats(&s)->rx_seq_jump, 3);
    CHECK_EQ(c.ev_count[STLV_EV_SEQ_GAP], 2);
    CHECK_EQ(c.last_lost, STLV_SEQ_GAP_MAX);
    CHECK_EQ(stlv_stats(&s)->rx_lost, 3 + STLV_SEQ_GAP_MAX);
    CHECK_EQ(c.ev_count[STLV_EV_PEER_RESET], 0);
}

/* 发送端 tx 发 n 帧，字节追加到 c->tx */
static void send_frames(stlv_t *tx, int n)
{
    for (int i = 0; i < n; i++) {
        CHECK_EQ(stlv_send_tlv(tx, (stlv_type_t)(0x40 + i), V, 2), STLV_OK);
    }
}

/* 从发送捕获中删除第 k 帧（从 0 计）；帧长固定为 fl */
static void drop_frame(cap_t *c, size_t k, size_t fl)
{
    memmove(c->tx + k * fl, c->tx + (k + 1) * fl, c->tx_len - (k + 1) * fl);
    c->tx_len -= fl;
}

static void test_peer_reset(void)
{
    const size_t fl = STLV_FRAME_SIZE(STLV_TLV_HDR_SIZE + 2);
    const int pre = 10; /* 重启前已发帧数，确保序号不是 0 */
    stlv_t tx, rx;
    cap_t ct, cr;

    /* 1. 发送端前 STLV_SYN_FRAMES 帧带 SYN，之后不带 */
    setup(&tx, &ct, NULL, 0);
    send_frames(&tx, STLV_SYN_FRAMES + 1);
    for (int i = 0; i <= STLV_SYN_FRAMES; i++) {
        uint8_t ctl = ct.tx[(size_t)i * fl + 2];
        CHECK_EQ(ctl & STLV_SEQ_MASK, i);
        CHECK_EQ((ctl & STLV_FLAG_SYN) != 0, i < STLV_SYN_FRAMES);
    }

    /* 2. 正常重启：识别为 PEER_RESET，不计丢帧，重启后的帧照常分发 */
    for (int lost_after = 0; lost_after < 3; lost_after++) {
        setup(&tx, &ct, NULL, 0);
        setup(&rx, &cr, NULL, 0);
        send_frames(&tx, pre);
        reinit(&tx, &ct); /* 对端重启 */
        send_frames(&tx, STLV_SYN_FRAMES + 2);
        /* 重启后的前 lost_after 帧（SYN 帧）丢失，只要还剩一个 SYN 帧就能识别 */
        for (int k = 0; k < lost_after; k++) {
            drop_frame(&ct, (size_t)pre, fl);
        }
        stlv_rx_feed(&rx, ct.tx, ct.tx_len);
        CHECK_EQ(stlv_stats(&rx)->rx_peer_reset, 1);
        CHECK_EQ(cr.ev_count[STLV_EV_PEER_RESET], 1);
        CHECK_EQ(stlv_stats(&rx)->rx_lost, 0);
        CHECK_EQ(stlv_stats(&rx)->rx_seq_jump, 0);
        CHECK_EQ(cr.n_tlv, pre + STLV_SYN_FRAMES + 2 - lost_after);
    }

    /* 3. 重启后同一启动序列内丢帧：不能误报重启，按丢帧统计 */
    if (STLV_SYN_FRAMES >= 3) {
        setup(&tx, &ct, NULL, 0);
        setup(&rx, &cr, NULL, 0);
        send_frames(&tx, pre);
        reinit(&tx, &ct);
        send_frames(&tx, STLV_SYN_FRAMES + 2);
        drop_frame(&ct, (size_t)pre + 1, fl); /* 新会话 seq1 丢失 */
        stlv_rx_feed(&rx, ct.tx, ct.tx_len);
        CHECK_EQ(stlv_stats(&rx)->rx_peer_reset, 1);
        CHECK_EQ(stlv_stats(&rx)->rx_lost, 1);
    }

    /* 4. 对端连续重启两次（第一次只来得及发 1 帧） */
    setup(&tx, &ct, NULL, 0);
    setup(&rx, &cr, NULL, 0);
    send_frames(&tx, pre);
    reinit(&tx, &ct);
    send_frames(&tx, 1);
    reinit(&tx, &ct);
    send_frames(&tx, 3);
    stlv_rx_feed(&rx, ct.tx, ct.tx_len);
    CHECK_EQ(stlv_stats(&rx)->rx_peer_reset, 2);
    CHECK_EQ(stlv_stats(&rx)->rx_lost, 0);

    /* 5. 重启时最后一帧只发出一半，紧跟 bootloader 打印的乱码，再接新会话 */
    setup(&tx, &ct, NULL, 0);
    setup(&rx, &cr, NULL, 0);
    send_frames(&tx, pre);
    ct.tx_len -= fl / 2; /* 截断最后一帧 */
    static const char boot_msg[] = "\r\nBootloader v1.2 \xA5\x5A\xA5 jumping to app...\r\n";
    memcpy(ct.tx + ct.tx_len, boot_msg, sizeof(boot_msg) - 1);
    ct.tx_len += sizeof(boot_msg) - 1;
    reinit(&tx, &ct);
    send_frames(&tx, 3);
    stlv_rx_feed(&rx, ct.tx, ct.tx_len);
    CHECK_EQ(stlv_stats(&rx)->rx_frames, (uint32_t)(pre - 1 + 3));
    CHECK_EQ(stlv_stats(&rx)->rx_peer_reset, 1);
    CHECK_EQ(stlv_stats(&rx)->rx_lost, 0);

    /* 6. 本端（接收端）自己刚启动，收到的第一帧就是 SYN：只建立基准，不报重启 */
    setup(&tx, &ct, NULL, 0);
    setup(&rx, &cr, NULL, 0);
    send_frames(&tx, 2);
    stlv_rx_feed(&rx, ct.tx, ct.tx_len);
    CHECK_EQ(stlv_stats(&rx)->rx_peer_reset, 0);
    CHECK_EQ(stlv_stats(&rx)->rx_frames, 2);

    /* 7. 已知局限：SYN 帧全部丢失时无法识别重启，退化为按序号差值判断 */
    setup(&tx, &ct, NULL, 0);
    setup(&rx, &cr, NULL, 0);
    send_frames(&tx, pre);
    reinit(&tx, &ct);
    send_frames(&tx, STLV_SYN_FRAMES + 1);
    for (int k = 0; k < STLV_SYN_FRAMES; k++) {
        drop_frame(&ct, (size_t)pre, fl);
    }
    stlv_rx_feed(&rx, ct.tx, ct.tx_len);
    CHECK_EQ(stlv_stats(&rx)->rx_peer_reset, 0);
    CHECK_EQ(stlv_stats(&rx)->rx_frames, (uint32_t)(pre + 1));
    /* 期望 pre，收到 STLV_SYN_FRAMES：差值 (SYN_FRAMES - pre) mod 128 */
    uint8_t gap = (uint8_t)((STLV_SYN_FRAMES - pre) & STLV_SEQ_MASK);
    if (gap <= STLV_SEQ_GAP_MAX) {
        CHECK_EQ(stlv_stats(&rx)->rx_lost, gap);
    } else {
        CHECK_EQ(stlv_stats(&rx)->rx_seq_jump, 1);
    }
}

static void test_format_error(void)
{
    /* CRC 正确但 TLV 结构损坏：整帧丢弃，不调用 on_tlv */
    uint8_t f[STLV_MAX_FRAME];
    stlv_builder_t b;
    stlv_build_begin(&b, f, sizeof(f));
    stlv_build_put(&b, 1, V, 4);
    f[STLV_HDR_SIZE + STLV_TYPE_SIZE] = 5; /* 声称 5 字节，实际 4 */
    size_t n = stlv_build_finish(&b, 9);

    stlv_t s;
    cap_t c;
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, f, n);
    CHECK_EQ(c.n_tlv, 0);
    CHECK_EQ(c.ev_count[STLV_EV_ERR_FORMAT], 1);
    CHECK_EQ(c.ev_count[STLV_EV_FRAME_BEGIN], 0);
    CHECK_EQ(stlv_stats(&s)->err_format, 1);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 0);
}

static void test_type_table(void)
{
    enum { T_PING = 1, T_TEMP = 2, T_NAME = 3 };
    static const stlv_type_desc_t types[] = {
        {T_PING, 0, 0},
        {T_TEMP, 2, 2},
        {T_NAME, 1, 8},
    };
    const stlv_tlv_t items[] = {
        {T_PING, 0, NULL},
        {T_TEMP, 2, V},
        {T_TEMP, 3, V},  /* 长度不符 */
        {T_NAME, 0, NULL}, /* 长度不符 */
        {T_NAME, 8, V},
        {9, 1, V},       /* 未知类型 */
    };
    uint8_t f[STLV_MAX_FRAME];
    size_t n = make_frame(f, 0, items, 6);

    stlv_t s;
    cap_t c;
    setup(&s, &c, types, 3);
    stlv_rx_feed(&s, f, n);
    CHECK_EQ(c.n_tlv, 3);
    CHECK_EQ(c.type[0], T_PING);
    CHECK_EQ(c.type[1], T_TEMP);
    CHECK_EQ(c.type[2], T_NAME);
    CHECK_EQ(c.ev_count[STLV_EV_TLV_BAD_LEN], 2);
    CHECK_EQ(c.ev_count[STLV_EV_TLV_UNKNOWN], 1);
    CHECK_EQ(stlv_stats(&s)->tlv_bad_len, 2);
    CHECK_EQ(stlv_stats(&s)->tlv_unknown, 1);
    CHECK_EQ(stlv_stats(&s)->rx_tlvs, 3);
}

static void test_send_paths(void)
{
    stlv_t s;
    cap_t c;
    setup(&s, &c, NULL, 0);

    /* send_tlv 与 builder 产生相同字节 */
    uint8_t ref[STLV_MAX_FRAME];
    size_t nref = make_frame(ref, STLV_FLAG_SYN | 0, ITEMS + 2, 1);
    CHECK_EQ(stlv_send_tlv(&s, ITEMS[2].type, ITEMS[2].value, ITEMS[2].len), STLV_OK);
    CHECK_EQ(c.tx_len, nref);
    CHECK_MEM(c.tx, ref, nref);

    /* stlv_send 使用递增序号 */
    uint8_t buf[STLV_MAX_FRAME];
    stlv_builder_t b;
    stlv_build_begin(&b, buf, sizeof(buf));
    stlv_build_put_items(&b, ITEMS, 3);
    CHECK_EQ(stlv_send(&s, &b), STLV_OK);
    CHECK_EQ(c.tx[nref + 2], STLV_SYN_FRAMES > 1 ? (STLV_FLAG_SYN | 1) : 1);
    CHECK_EQ(stlv_stats(&s)->tx_frames, 2);

    /* 写失败：返回 IO，序号仍然前进 */
    c.tx_fail = 1;
    CHECK_EQ(stlv_send(&s, &b), STLV_ERR_IO);
    CHECK_EQ(stlv_send_tlv(&s, 1, NULL, 0), STLV_ERR_IO);
    c.tx_fail = 0;
    size_t before = c.tx_len;
    CHECK_EQ(stlv_send_tlv(&s, 1, NULL, 0), STLV_OK);
    CHECK_EQ(c.tx[before + 2] & STLV_SEQ_MASK, 4);
    CHECK_EQ(stlv_stats(&s)->tx_frames, 3);

    /* 超长单 TLV */
    static uint8_t big[STLV_MAX_PAYLOAD];
    size_t too_long = STLV_MAX_PAYLOAD - STLV_TLV_HDR_SIZE + 1;
    if (too_long <= STLV_VALUE_MAX) {
        CHECK_EQ(stlv_send_tlv(&s, 1, big, too_long), STLV_ERR_NOSPC);
    }
    CHECK_EQ(stlv_send_tlv(&s, 1, NULL, 1), STLV_ERR_PARAM);

    /* 无效构建器 */
    stlv_builder_t bad;
    stlv_build_begin(&bad, NULL, 0);
    CHECK_EQ(stlv_send(&s, &bad), STLV_ERR_PARAM);
}

static void test_echo_from_callback(void)
{
    /* A 收到 TLV 后在回调里发给 B（回调内发送是允许的） */
    stlv_t a, b;
    cap_t ca, cb;
    setup(&a, &ca, NULL, 0);
    setup(&b, &cb, NULL, 0);
    ca.peer = &a; /* A 的回调用 A 自身发送，写入 ca.tx */

    uint8_t f[STLV_MAX_FRAME];
    size_t n = make_frame(f, 0, ITEMS, 3);
    stlv_rx_feed(&a, f, n);
    CHECK_EQ(stlv_stats(&a)->tx_frames, 3);

    stlv_rx_feed(&b, ca.tx, ca.tx_len);
    CHECK_EQ(stlv_stats(&b)->rx_frames, 3);
    CHECK_EQ(cb.n_tlv, 3);
    CHECK_EQ(cb.len[2], 8);
    CHECK_EQ(cb.ev_count[STLV_EV_SEQ_GAP], 0);
}

static void test_max_payload_frame(void)
{
    static uint8_t val[STLV_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(val); i++) {
        val[i] = (uint8_t)(i * 7u + 1u);
    }
    uint8_t f[STLV_MAX_FRAME];
    stlv_builder_t b;
    stlv_build_begin(&b, f, sizeof(f));
    while (b.len + STLV_TLV_HDR_SIZE < STLV_MAX_PAYLOAD) {
        size_t v = STLV_MAX_PAYLOAD - b.len - STLV_TLV_HDR_SIZE;
        if (v > STLV_VALUE_MAX) {
            v = STLV_VALUE_MAX;
        }
        CHECK_EQ(stlv_build_put(&b, 1, val, v), STLV_OK);
    }
    size_t n = stlv_build_finish(&b, 0);
    CHECK(n >= STLV_MAX_FRAME - STLV_TLV_HDR_SIZE);

    stlv_t s;
    cap_t c;
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, f, n);
    stlv_rx_feed(&s, f, n);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 2);
    CHECK_EQ(stlv_rx_feed(&s, NULL, 1), STLV_ERR_PARAM);
    CHECK_EQ(stlv_rx_feed(&s, NULL, 0), STLV_OK);
}

static void test_reinit_from_callback(void)
{
    /* 回调中重新 init：不能越界；已缓冲的数据随复位丢弃，之后喂入的数据正常处理 */
    uint8_t stream[3 * STLV_MAX_FRAME];
    size_t n = make_frame(stream, 0, ITEMS, 3);
    n += make_frame(stream + n, 1, ITEMS, 3);
    n += make_frame(stream + n, 2, ITEMS, 1);

    stlv_t s;
    cap_t c;
    setup(&s, &c, NULL, 0);
    c.reinit_at = 2; /* 第一帧第二个 TLV 时复位 */
    CHECK_EQ(stlv_rx_feed(&s, stream, n), STLV_OK);
    /* 同一次 feed 中已缓冲的后续帧随复位丢弃；缓冲区较小时后续帧在复位后才拷入，会被处理 */
    CHECK(c.n_tlv >= 2 && c.n_tlv <= 2 + 3 + 1);
    CHECK_EQ(s.rx_len, 0);
    int before = c.n_tlv;
    uint32_t frames_before = stlv_stats(&s)->rx_frames;
    CHECK_EQ(stlv_rx_feed(&s, stream, n), STLV_OK); /* 复位后实例可正常使用 */
    CHECK_EQ(c.n_tlv - before, 3 + 3 + 1);
    CHECK_EQ(stlv_stats(&s)->rx_frames - frames_before, 3);

    /* 逐字节喂入：复位后的字节由新状态处理 */
    setup(&s, &c, NULL, 0);
    c.reinit_at = 2;
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(stlv_rx_feed(&s, stream + i, 1), STLV_OK);
    }
    CHECK_EQ(c.n_tlv, 2 + 3 + 1); /* 第一帧剩余 TLV 被放弃 */
    CHECK_EQ(stlv_stats(&s)->rx_frames, 2);
    CHECK_EQ(stlv_stats(&s)->rx_lost, 0);

    setup(&s, &c, NULL, 0);
    c.reinit_on_begin = 1;
    for (size_t i = 0; i < n; i++) {
        stlv_rx_feed(&s, stream + i, 1);
    }
    CHECK_EQ(c.n_tlv, 3 + 1); /* 第一帧整帧放弃 */
    CHECK_EQ(stlv_stats(&s)->rx_frames, 2);
}

static void test_partial_write_failure(void)
{
    /* send_tlv 写完帧头后失败：接收端看到半帧，之后的帧仍能恢复，并报告丢 1 帧 */
    stlv_t tx, rx;
    cap_t ct, cr;
    setup(&tx, &ct, NULL, 0);
    setup(&rx, &cr, NULL, 0);
    CHECK_EQ(stlv_send_tlv(&tx, 1, V, 8), STLV_OK);
    ct.fail_write_at = ct.tx_calls + 2; /* 第二段（value）失败 */
    CHECK_EQ(stlv_send_tlv(&tx, 2, V, 8), STLV_ERR_IO);
    CHECK_EQ(stlv_send_tlv(&tx, 3, V, 8), STLV_OK);

    stlv_rx_feed(&rx, ct.tx, ct.tx_len);
    stlv_rx_flush(&rx);
    CHECK_EQ(stlv_stats(&rx)->rx_frames, 2);
    CHECK_EQ(cr.type[1], 3);
    CHECK_EQ(stlv_stats(&rx)->rx_lost, 1);
}

static void test_sof_flood_is_linear(void)
{
    /* 大量 SOF0 字节逐字节触发重扫；读指针实现下拷贝量应与输入线性相关 */
    static uint8_t flood[8 * STLV_MAX_FRAME];
    memset(flood, STLV_SOF0, sizeof(flood));
    uint8_t f[STLV_MAX_FRAME];
    size_t n = make_frame(f, 0, ITEMS, 3);

    stlv_t s;
    cap_t c;
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, flood, sizeof(flood));
    stlv_rx_feed(&s, f, n);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 1);
    CHECK_EQ(stlv_stats(&s)->rx_discarded, sizeof(flood));

    /* 坏帧头声称最大长度，payload 全是 SOF0 */
    static uint8_t bad[STLV_MAX_FRAME + STLV_MAX_FRAME];
    memset(bad, STLV_SOF0, sizeof(bad));
    bad[1] = STLV_SOF1;
    bad[2] = 0;
    bad[3] = (uint8_t)STLV_MAX_PAYLOAD;
    bad[4] = (uint8_t)(STLV_MAX_PAYLOAD >> 8);
    bad[5] = stlv_crc8(0, bad + 2, 3);
    setup(&s, &c, NULL, 0);
    stlv_rx_feed(&s, bad, sizeof(bad));
    stlv_rx_feed(&s, f, n);
    stlv_rx_flush(&s);
    CHECK_EQ(stlv_stats(&s)->rx_frames, 1);
}

void suite_rx(void)
{
    RUN_TEST(test_init_params);
    RUN_TEST(test_feed_whole_and_bytewise);
    RUN_TEST(test_multi_frames_and_garbage);
    RUN_TEST(test_payload_contains_sof);
    RUN_TEST(test_bad_frames_recover);
    RUN_TEST(test_flush_releases_waiting_frame);
    RUN_TEST(test_seq_tracking);
    RUN_TEST(test_peer_reset);
    RUN_TEST(test_format_error);
    RUN_TEST(test_type_table);
    RUN_TEST(test_send_paths);
    RUN_TEST(test_echo_from_callback);
    RUN_TEST(test_max_payload_frame);
    RUN_TEST(test_reinit_from_callback);
    RUN_TEST(test_partial_write_failure);
    RUN_TEST(test_sof_flood_is_linear);
}
