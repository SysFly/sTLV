/*
 * 故障注入测试：发送端产生确定性的帧序列，经过有损信道（丢帧、丢字节、插入字节、
 * 比特翻转、截断、帧间噪声/假帧头），发送端还会随机"重启"（序号归零、重新发 SYN，
 * 重启瞬间正在发的帧可能被截断，并插入 bootloader 乱码）。字节流以随机分块喂给接收端，
 * 并在随机的帧间空闲点调用 stlv_rx_flush()。断言：
 *   1. 所有交付的帧内容与发送内容完全一致（无误收）；
 *   2. 交付帧序列 == 预言机根据最终字节流推出的完好帧序列（好帧一个不丢）；
 *   3. 对每对相邻交付帧，预言机按真实会话独立推出期望结果：同一会话内丢了几帧、
 *      跨会话时是否可识别为对端重启（可识别 => 必须报 PEER_RESET 且不计丢帧；
 *      SYN 帧全丢等已知局限 => 按序号差值退化处理），统计值必须完全一致；
 *   4. 丢弃字节数 + 有效帧字节数 == 信道输出总字节数。
 *
 * 环境变量 STLV_FAULT_SEED / STLV_FAULT_FRAMES 可覆盖种子和帧数做长时间浸泡测试。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stlv.h"
#include "tinytest.h"

#define T_INDEX 0 /* 每帧第一个 TLV：32 位帧编号 */

typedef struct {
    uint32_t s;
} rng_t;

static uint32_t rnd(rng_t *r)
{
    /* xorshift32 */
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

static uint32_t rnd_below(rng_t *r, uint32_t n)
{
    return n ? rnd(r) % n : 0;
}

/* 概率以千分比表示 */
static int chance(rng_t *r, unsigned permille)
{
    return rnd_below(r, 1000) < permille;
}

typedef struct {
    const char *name;
    unsigned drop_frame;  /* 整帧丢失 */
    unsigned drop_byte;   /* 帧内丢一个字节 */
    unsigned dup_byte;    /* 帧内多出一个字节 */
    unsigned flip_bit;    /* 帧内翻转 1~3 个比特 */
    unsigned truncate;    /* 帧被截断 */
    unsigned noise;       /* 帧后插入噪声（不损坏帧本身） */
    unsigned idle;        /* 帧后出现空闲，接收端 flush */
    unsigned reboot;      /* 发送此帧前发送端重启 */
} scenario_t;

/* 向构建器写入帧 idx 的确定性 payload */
static void gen_payload(uint32_t seed, uint32_t idx, uint8_t *frame_buf, stlv_builder_t *b)
{
    rng_t r = {seed ^ (idx * 0x9E3779B9u) ^ 0x5bd1e995u};
    if (r.s == 0) {
        r.s = 1;
    }
    stlv_build_begin(b, frame_buf, STLV_MAX_FRAME);
    stlv_build_put_u32(b, T_INDEX, idx);

    uint8_t val[STLV_MAX_PAYLOAD];
    unsigned n = rnd_below(&r, 6);
    for (unsigned k = 0; k < n; k++) {
        size_t room = STLV_MAX_PAYLOAD - b->len;
        if (room < STLV_TLV_HDR_SIZE) {
            break;
        }
        size_t max_v = room - STLV_TLV_HDR_SIZE;
        if (max_v > STLV_VALUE_MAX) {
            max_v = STLV_VALUE_MAX;
        }
        /* 偏向短 Value，偶尔用满 */
        size_t vlen = chance(&r, 100) ? max_v : rnd_below(&r, (uint32_t)(max_v < 24 ? max_v + 1 : 24));
        for (size_t i = 0; i < vlen; i++) {
            /* 刻意多放 SOF 字节，制造假帧头 */
            val[i] = chance(&r, 150) ? (uint8_t)(i & 1 ? STLV_SOF1 : STLV_SOF0) : (uint8_t)rnd(&r);
        }
        stlv_type_t t = (stlv_type_t)rnd(&r);
        stlv_build_put(b, t, val, vlen);
    }
}

/* ---------------------------------------------------------------------- */
/* 接收端校验 */

typedef struct {
    uint32_t seed;
    const uint8_t *ctl; /* 每帧发送时的 CTL 字节 */
    uint8_t rebuilt[STLV_MAX_FRAME];
    stlv_builder_t b;
    uint32_t *delivered;
    size_t n_delivered;
    size_t cap_delivered;
    uint32_t false_accepts;
    uint64_t delivered_bytes;
} rx_check_t;

static void fault_on_tlv(stlv_t *ctx, const stlv_tlv_t *tlv, void *user)
{
    (void)ctx;
    rx_check_t *c = (rx_check_t *)user;
    stlv_build_put(&c->b, tlv->type, tlv->value, tlv->len);
}

static void fault_on_event(stlv_t *ctx, stlv_event_t ev, const stlv_event_info_t *info, void *user)
{
    (void)ctx;
    rx_check_t *c = (rx_check_t *)user;
    if (ev == STLV_EV_FRAME_BEGIN) {
        stlv_build_begin(&c->b, c->rebuilt, sizeof(c->rebuilt));
    } else if (ev == STLV_EV_FRAME_END) {
        c->delivered_bytes += STLV_FRAME_SIZE(info->payload_len);
        stlv_iter_t it;
        stlv_iter_init(&it, stlv_build_payload(&c->b), c->b.len);
        if (!stlv_iter_next(&it) || it.tlv.type != T_INDEX || it.tlv.len != 4) {
            c->false_accepts++;
            return;
        }
        uint32_t idx = stlv_rd_u32(it.tlv.value);
        uint8_t expect[STLV_MAX_FRAME];
        stlv_builder_t eb;
        gen_payload(c->seed, idx, expect, &eb);
        if (eb.len != c->b.len || info->seq != (c->ctl[idx] & STLV_SEQ_MASK) ||
            memcmp(stlv_build_payload(&eb), stlv_build_payload(&c->b), eb.len) != 0) {
            c->false_accepts++;
            return;
        }
        if (c->n_delivered < c->cap_delivered) {
            c->delivered[c->n_delivered++] = idx;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* 信道 */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t *idle_at; /* 空闲点（字节偏移），升序 */
    size_t n_idle;
} stream_t;

static void put_bytes(stream_t *s, const uint8_t *p, size_t n)
{
    if (s->len + n > s->cap) {
        s->cap = (s->len + n) * 2;
        s->data = (uint8_t *)realloc(s->data, s->cap);
        if (s->data == NULL) {
            abort();
        }
    }
    memcpy(s->data + s->len, p, n);
    s->len += n;
}

#define NOT_SENT ((size_t)-1)

/* 对一帧施加信道故障并写入输出流；返回帧在流中的起始偏移，整帧丢失返回 NOT_SENT */
static size_t channel(rng_t *r, const scenario_t *sc, const uint8_t *f, size_t n, stream_t *out,
                      unsigned *corruptions)
{
    if (chance(r, sc->drop_frame)) {
        return NOT_SENT;
    }
    uint8_t tmp[STLV_MAX_FRAME + 1];
    size_t n_out = n;
    memcpy(tmp, f, n);

    if (chance(r, sc->flip_bit)) {
        unsigned k = 1 + rnd_below(r, 3);
        for (unsigned i = 0; i < k; i++) {
            size_t pos = rnd_below(r, (uint32_t)n);
            tmp[pos] ^= (uint8_t)(1u << rnd_below(r, 8));
        }
    }
    if (chance(r, sc->drop_byte)) {
        size_t pos = rnd_below(r, (uint32_t)n);
        memmove(tmp + pos, tmp + pos + 1, n - pos - 1);
        n_out--;
    } else if (chance(r, sc->dup_byte)) {
        size_t pos = rnd_below(r, (uint32_t)n);
        memmove(tmp + pos + 1, tmp + pos, n - pos);
        n_out++;
    } else if (chance(r, sc->truncate)) {
        n_out = 1 + rnd_below(r, (uint32_t)(n - 1));
    }
    if (n_out != n || memcmp(tmp, f, n) != 0) {
        (*corruptions)++;
    }
    size_t start = out->len;
    put_bytes(out, tmp, n_out);
    return start;
}

static void add_noise(rng_t *r, stream_t *out)
{
    uint8_t junk[48];
    size_t n = 1 + rnd_below(r, sizeof(junk));
    for (size_t i = 0; i < n; i++) {
        junk[i] = (uint8_t)rnd(r);
    }
    /* 一半概率伪造一个以 SOF 开头的假帧头 */
    if (n >= 6 && chance(r, 500)) {
        junk[0] = STLV_SOF0;
        junk[1] = STLV_SOF1;
        if (chance(r, 500)) {
            junk[5] = stlv_crc8(0, junk + 2, 3); /* 帧头校验也正确 */
        }
    }
    put_bytes(out, junk, n);
}

/* (a, a+n) 内部是否有空闲点（接收端会在此 flush，跨越空闲点的帧收不到） */
static int spans_idle(const stream_t *st, size_t a, size_t n)
{
    size_t lo = 0, hi = st->n_idle; /* 找第一个 > a 的空闲点 */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (st->idle_at[mid] <= a) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < st->n_idle && st->idle_at[lo] < a + n;
}

/*
 * 预言机：在最终字节流上计算接收端应交付的帧。
 * 帧 i 的原始字节若仍连续出现在起点 start-1..start+1 处（复制首字节会右移 1；
 * 丢掉首/末字节而相邻字节恰好相同、截断后噪声恰好补齐，都会让帧"复原"），
 * 且不跨越空闲点，即为候选；
 * 再按从左到右、互不重叠贪心选取（一个字节只能属于一帧）。
 */
static size_t oracle(uint32_t seed, uint32_t frames, const size_t *start, const uint8_t *ctl,
                     const stream_t *st, uint32_t *expect, uint64_t *expect_bytes)
{
    size_t n_expect = 0, cursor = 0;
    *expect_bytes = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (start[i] == NOT_SENT) {
            continue;
        }
        uint8_t f[STLV_MAX_FRAME];
        stlv_builder_t b;
        gen_payload(seed, i, f, &b);
        size_t n = stlv_build_finish(&b, ctl[i]);
        size_t first = start[i] > cursor ? start[i] - 1 : cursor;
        for (size_t off = first; off <= start[i] + 1 && off + n <= st->len; off++) {
            if (off >= cursor && memcmp(st->data + off, f, n) == 0 &&
                !spans_idle(st, off, n)) {
                expect[n_expect++] = i;
                *expect_bytes += n;
                cursor = off + n;
                break;
            }
        }
    }
    return n_expect;
}

typedef struct {
    uint32_t lost;       /* 期望 rx_lost */
    uint32_t reset;      /* 期望 rx_peer_reset */
    uint32_t jump;       /* 期望 rx_seq_jump */
    uint32_t reboots;    /* 交付帧之间真实发生的会话切换 */
    uint32_t undetected; /* 其中按协议无法识别的（已知局限） */
} seq_expect_t;

/*
 * 根据真实会话推出期望的序号统计。同一会话内：丢帧数就是两帧编号之差减 1。
 * 跨会话：新帧带 SYN 且不像"同一启动序列的后续帧"（上一帧带 SYN 且序号更小）时，
 * 协议保证可识别；否则属于已知局限，按 7 位序号差值退化处理。
 */
static seq_expect_t seq_oracle(const uint32_t *expect, size_t n_expect, const uint8_t *ctl,
                               const uint32_t *sess)
{
    seq_expect_t e = {0, 0, 0, 0, 0};
    for (size_t k = 1; k < n_expect; k++) {
        uint32_t a = expect[k - 1], b = expect[k];
        unsigned sa = ctl[a] & STLV_SEQ_MASK, sb = ctl[b] & STLV_SEQ_MASK;
        int syn_a = (ctl[a] & STLV_FLAG_SYN) != 0, syn_b = (ctl[b] & STLV_FLAG_SYN) != 0;
        if (sess[a] == sess[b]) {
            CHECK(b - a - 1 <= STLV_SEQ_GAP_MAX); /* 场景须保证同会话内连续丢帧不超限 */
            e.lost += b - a - 1;
            continue;
        }
        e.reboots++;
        if (syn_b && !(syn_a && sa < sb)) {
            e.reset++;
            continue;
        }
        e.undetected++;
        unsigned gap = (sb - sa - 1u) & STLV_SEQ_MASK;
        if (gap != 0 && gap <= STLV_SEQ_GAP_MAX) {
            e.lost += gap;
        } else if (gap != 0) {
            e.jump++;
        }
    }
    return e;
}

/* ---------------------------------------------------------------------- */

static void run_scenario(const scenario_t *sc, uint32_t seed, uint32_t frames)
{
    rng_t r = {seed};
    stream_t st = {0};
    size_t *start = (size_t *)malloc(sizeof(size_t) * frames);
    uint32_t *expect = (uint32_t *)malloc(sizeof(uint32_t) * frames);
    uint8_t *ctl = (uint8_t *)malloc(frames);
    uint32_t *sess = (uint32_t *)malloc(sizeof(uint32_t) * frames);
    unsigned corruptions = 0;

    /* 1. 发送端 + 信道；发送端序号/SYN 规则与库一致，但在这里独立实现 */
    st.idle_at = (size_t *)malloc(sizeof(size_t) * (frames + 1));
    uint32_t cur_sess = 0;
    unsigned tx_seq = 0, syn_left = STLV_SYN_FRAMES;
    size_t prev_end = 0, prev_n = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (i != 0 && chance(&r, sc->reboot)) {
            cur_sess++;
            tx_seq = 0;
            syn_left = STLV_SYN_FRAMES;
            /* 一半概率：重启时上一帧还没发完 */
            if (prev_n > 1 && st.len == prev_end && chance(&r, 500)) {
                st.len -= 1 + rnd_below(&r, (uint32_t)(prev_n - 1));
                while (st.n_idle != 0 && st.idle_at[st.n_idle - 1] > st.len) {
                    st.n_idle--;
                }
            }
            add_noise(&r, &st); /* bootloader 打印 */
        }
        ctl[i] = (uint8_t)(tx_seq | (syn_left != 0 ? STLV_FLAG_SYN : 0u));
        sess[i] = cur_sess;
        tx_seq = (tx_seq + 1u) & STLV_SEQ_MASK;
        syn_left -= syn_left != 0;

        uint8_t f[STLV_MAX_FRAME];
        stlv_builder_t b;
        gen_payload(seed, i, f, &b);
        size_t n = stlv_build_finish(&b, ctl[i]);

        start[i] = channel(&r, sc, f, n, &st, &corruptions);
        prev_end = st.len;
        prev_n = start[i] == NOT_SENT ? 0 : st.len - start[i];
        if (chance(&r, sc->noise)) {
            add_noise(&r, &st);
        }
        if (chance(&r, sc->idle)) {
            st.idle_at[st.n_idle++] = st.len;
        }
    }
    uint64_t expect_bytes;
    size_t n_expect = oracle(seed, frames, start, ctl, &st, expect, &expect_bytes);
    seq_expect_t se = seq_oracle(expect, n_expect, ctl, sess);

    /* 2. 接收端：随机分块喂入，遇到空闲点 flush */
    rx_check_t chk;
    memset(&chk, 0, sizeof(chk));
    chk.seed = seed;
    chk.ctl = ctl;
    chk.cap_delivered = frames;
    chk.delivered = (uint32_t *)malloc(sizeof(uint32_t) * frames);

    stlv_t rx;
    stlv_cfg_t cfg = {NULL, fault_on_tlv, fault_on_event, NULL, 0, &chk};
    CHECK_EQ(stlv_init(&rx, &cfg), STLV_OK);

    size_t pos = 0, next_idle = 0;
    while (pos < st.len) {
        size_t limit = next_idle < st.n_idle ? st.idle_at[next_idle] : st.len;
        size_t k = 1 + rnd_below(&r, 64);
        if (pos + k > limit) {
            k = limit - pos;
        }
        stlv_rx_feed(&rx, st.data + pos, k);
        pos += k;
        while (next_idle < st.n_idle && st.idle_at[next_idle] == pos) {
            stlv_rx_flush(&rx);
            next_idle++;
        }
    }
    stlv_rx_flush(&rx);
    const stlv_stats_t *s = stlv_stats(&rx);

    /* 3. 断言 */
    /* CRC16 对随机候选帧的漏检率约 1/65536；按 CRC 校验次数估计期望值并留足余量 */
    unsigned max_false = 2u + (unsigned)(s->err_crc / 4096u);
    CHECK(chk.false_accepts <= max_false);
    CHECK_EQ(s->rx_frames, chk.n_delivered + chk.false_accepts);
    if (chk.false_accepts == 0) {
        CHECK_EQ(chk.n_delivered, n_expect);
        size_t m = chk.n_delivered < n_expect ? chk.n_delivered : n_expect;
        size_t mismatch = 0;
        for (size_t i = 0; i < m; i++) {
            mismatch += chk.delivered[i] != expect[i];
        }
        CHECK_EQ(mismatch, 0);
        for (size_t i = 0; i < m; i++) {
            if (chk.delivered[i] != expect[i]) {
                uint32_t e = expect[i];
                printf("    first mismatch at #%zu: expect frame %u (start=%zu) got frame %u\n", i,
                       (unsigned)e, start[e], (unsigned)chk.delivered[i]);
                break;
            }
        }

        CHECK_EQ(s->rx_lost, se.lost);
        CHECK_EQ(s->rx_peer_reset, se.reset);
        CHECK_EQ(s->rx_seq_jump, se.jump);
        CHECK_EQ(s->rx_discarded + expect_bytes, st.len);
        CHECK_EQ(chk.delivered_bytes, expect_bytes);
    } else {
        /* 误收可能吞掉其后的一个好帧 */
        CHECK(chk.n_delivered + 2 * chk.false_accepts >= n_expect);
    }
    CHECK_EQ(rx.rx_len, 0);
    if (n_expect == frames) {
        CHECK_EQ(s->rx_lost, 0);
        if (sc->noise == 0) {
            CHECK_EQ(s->err_crc, 0);
            CHECK_EQ(s->err_header, 0);
            CHECK_EQ(s->rx_discarded, 0);
        }
    }

    printf("    %-12s seed=%08x frames=%u expect=%zu corrupted=%u lost(seq)=%u "
           "crc_err=%u hdr_err=%u discarded=%u reboot=%u/%u(undetectable=%u) "
           "false_accept=%u\n",
           sc->name, (unsigned)seed, (unsigned)frames, n_expect, corruptions,
           (unsigned)s->rx_lost, (unsigned)s->err_crc, (unsigned)s->err_header,
           (unsigned)s->rx_discarded, (unsigned)s->rx_peer_reset, (unsigned)se.reboots,
           (unsigned)se.undetected, (unsigned)chk.false_accepts);

    free(st.data);
    free(st.idle_at);
    free(start);
    free(expect);
    free(ctl);
    free(sess);
    free(chk.delivered);
}

static const scenario_t SCENARIOS[] = {
    /* name          drop  dropB dupB  flip  trunc noise idle  reboot */
    {"clean",        0,    0,    0,    0,    0,    0,    0,    0},
    {"clean-idle",   0,    0,    0,    0,    0,    0,    300,  0},
    {"frame-loss",   150,  0,    0,    0,    0,    0,    0,    0},
    {"byte-loss",    0,    100,  0,    0,    0,    0,    50,   0},
    {"byte-dup",     0,    0,    100,  0,    0,    0,    50,   0},
    {"bit-flip",     0,    0,    0,    150,  0,    0,    0,    0},
    {"truncate",     0,    0,    0,    0,    100,  0,    200,  0},
    {"noise",        0,    0,    0,    0,    0,    300,  100,  0},
    {"mixed",        50,   50,   30,   50,   30,   100,  100,  0},
    {"burst",        300,  150,  50,   100,  100,  300,  50,   0},
    {"reboot",       0,    0,    0,    0,    0,    0,    100,  20},
    {"reboot-lossy", 100,  30,   30,   50,   30,   100,  100,  20},
    {"reboot-storm", 300,  50,   50,   100,  50,   200,  50,   150},
};

static void test_fault_injection(void)
{
    uint32_t seed = 0x1234567u;
    uint32_t frames = 2000;
    const char *e = getenv("STLV_FAULT_SEED");
    if (e != NULL) {
        seed = (uint32_t)strtoul(e, NULL, 0);
    }
    e = getenv("STLV_FAULT_FRAMES");
    if (e != NULL) {
        frames = (uint32_t)strtoul(e, NULL, 0);
    }
    for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); i++) {
        run_scenario(&SCENARIOS[i], seed + (uint32_t)i * 7919u, frames);
    }
}

void suite_fault(void)
{
    RUN_TEST(test_fault_injection);
}
