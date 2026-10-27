/* CRC、封包、拆包遍历测试 */
#include <string.h>

#include "stlv.h"
#include "tinytest.h"

static void test_crc_vectors(void)
{
    CHECK_EQ(stlv_crc16(0xFFFF, "123456789", 9), 0x29B1);
    CHECK_EQ(stlv_crc8(0, "123456789", 9), 0xF4);
    /* 增量计算等价于一次计算 */
    uint16_t c = stlv_crc16(0xFFFF, "1234", 4);
    CHECK_EQ(stlv_crc16(c, "56789", 5), 0x29B1);
    CHECK_EQ(stlv_crc16(0xFFFF, NULL, 0), 0xFFFF);
}

static void test_golden_frame(void)
{
    uint8_t buf[64];
    stlv_builder_t b;
    CHECK_EQ(stlv_build_begin(&b, buf, sizeof(buf)), STLV_OK);

    static const uint8_t empty[] = {0xA5, 0x5A, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x84};
    CHECK_EQ(stlv_build_finish(&b, 0), sizeof(empty));
    CHECK_MEM(buf, empty, sizeof(empty));

#if STLV_TYPE_SIZE == 1 && STLV_LEN_SIZE == 1
    static const uint8_t golden[] = {0xA5, 0x5A, 0x10, 0x09, 0x00, 0x1F, 0x01, 0x01, 0xAA,
                                     0x7F, 0x00, 0x02, 0x02, 0x34, 0x12, 0x46, 0x86};
    uint8_t aa = 0xAA;
    CHECK_EQ(stlv_build_put(&b, 0x01, &aa, 1), STLV_OK);
    CHECK_EQ(stlv_build_put(&b, 0x7F, NULL, 0), STLV_OK);
    CHECK_EQ(stlv_build_put_u16(&b, 0x02, 0x1234), STLV_OK);
    CHECK_EQ(stlv_build_finish(&b, 0x10), sizeof(golden));
    CHECK_MEM(buf, golden, sizeof(golden));
#elif STLV_TYPE_SIZE == 2 && STLV_LEN_SIZE == 2
    static const uint8_t golden[] = {0xA5, 0x5A, 0xFF, 0x09, 0x00, 0x96, 0x02, 0x01, 0x01,
                                     0x00, 0xAA, 0xEF, 0xBE, 0x00, 0x00, 0xD7, 0xC6};
    CHECK_EQ(stlv_build_put_u8(&b, 0x0102, 0xAA), STLV_OK);
    CHECK_EQ(stlv_build_put(&b, 0xBEEF, NULL, 0), STLV_OK);
    CHECK_EQ(stlv_build_finish(&b, 0xFF), sizeof(golden));
    CHECK_MEM(buf, golden, sizeof(golden));
#endif
}

static void test_build_and_foreach(void)
{
    uint8_t buf[STLV_MAX_FRAME];
    stlv_builder_t b;
    stlv_build_begin(&b, buf, sizeof(buf));

    static const uint8_t blob[5] = {1, 2, 3, 4, 5};
    const stlv_tlv_t items[] = {
        {0x10, sizeof(blob), blob},
        {0x11, 0, NULL},
        {0x12, 1, blob + 4},
    };
    CHECK_EQ(stlv_build_put_items(&b, items, 3), STLV_OK);
    CHECK_EQ(stlv_build_put_u32(&b, 0x13, 0xDEADBEEFu), STLV_OK);
    CHECK_EQ(b.len, 4 * STLV_TLV_HDR_SIZE + 5 + 0 + 1 + 4);

    /* 封包侧遍历 */
    stlv_iter_t it;
    int n = 0;
    STLV_BUILD_FOREACH(it, &b) {
        if (n < 3) {
            CHECK_EQ(it.tlv.type, items[n].type);
            CHECK_EQ(it.tlv.len, items[n].len);
            CHECK(items[n].len == 0 || memcmp(it.tlv.value, items[n].value, it.tlv.len) == 0);
        } else {
            CHECK_EQ(it.tlv.type, 0x13);
            CHECK_EQ(it.tlv.len, 4);
            CHECK_EQ(stlv_rd_u32(it.tlv.value), 0xDEADBEEFu);
        }
        n++;
    }
    CHECK_EQ(n, 4);
    CHECK_EQ(it.err, STLV_OK);
    CHECK_EQ(stlv_validate(stlv_build_payload(&b), b.len), 4);

    /* 提前 break */
    n = 0;
    STLV_BUILD_FOREACH(it, &b) {
        if (++n == 2) {
            break;
        }
    }
    CHECK_EQ(n, 2);
}

static void test_iter_edge_cases(void)
{
    stlv_iter_t it;
    int n = 0;
    STLV_FOREACH(it, NULL, 0) {
        n++;
    }
    CHECK_EQ(n, 0);
    CHECK_EQ(it.err, STLV_OK);
    CHECK_EQ(stlv_validate(NULL, 0), 0);

    /* 不足一个 TLV 头 */
    uint8_t p[16] = {0};
    CHECK_EQ(stlv_validate(p, STLV_TLV_HDR_SIZE - 1), STLV_ERR_FORMAT);

    /* 一个完整 TLV + 截断的 TLV：遍历先给出第一个，再报错 */
    memset(p, 0, sizeof(p));
    p[STLV_TYPE_SIZE] = 1;                        /* len = 1 */
    p[STLV_TLV_HDR_SIZE + 1 + STLV_TYPE_SIZE] = 5; /* 第二个声称 5 字节 */
    size_t plen = STLV_TLV_HDR_SIZE + 1 + STLV_TLV_HDR_SIZE + 2;
    n = 0;
    STLV_FOREACH(it, p, plen) {
        n++;
    }
    CHECK_EQ(n, 1);
    CHECK_EQ(it.err, STLV_ERR_FORMAT);
    CHECK_EQ(stlv_validate(p, plen), STLV_ERR_FORMAT);
}

static void test_build_limits(void)
{
    uint8_t buf[STLV_MAX_FRAME + 16];
    static uint8_t big[STLV_MAX_PAYLOAD + 16];
    stlv_builder_t b;

    /* 缓冲区过小 */
    CHECK_EQ(stlv_build_begin(&b, buf, STLV_FRAME_OVERHEAD - 1), STLV_ERR_PARAM);
    CHECK_EQ(stlv_build_put_u8(&b, 1, 1), STLV_ERR_PARAM);
    CHECK_EQ(stlv_build_finish(&b, 0), 0);
    CHECK_EQ(stlv_build_begin(&b, NULL, 100), STLV_ERR_PARAM);

    /* 容量受 cap 限制，失败不改变构建器 */
    stlv_build_begin(&b, buf, STLV_FRAME_SIZE(STLV_TLV_HDR_SIZE + 1));
    CHECK_EQ(stlv_build_put(&b, 1, big, 2), STLV_ERR_NOSPC);
    CHECK_EQ(b.len, 0);
    CHECK_EQ(stlv_build_put(&b, 1, big, 1), STLV_OK);
    CHECK_EQ(stlv_build_put(&b, 1, NULL, 0), STLV_ERR_NOSPC);
    CHECK_EQ(b.len, STLV_TLV_HDR_SIZE + 1);

    /* 容量受 STLV_MAX_PAYLOAD 限制：恰好填满 */
    stlv_build_begin(&b, buf, sizeof(buf));
    size_t v = STLV_MAX_PAYLOAD - STLV_TLV_HDR_SIZE;
    if (v > STLV_VALUE_MAX) {
        v = STLV_VALUE_MAX;
    }
    while (b.len + STLV_TLV_HDR_SIZE + v <= STLV_MAX_PAYLOAD) {
        CHECK_EQ(stlv_build_put(&b, 2, big, v), STLV_OK);
    }
    size_t rest = STLV_MAX_PAYLOAD - b.len;
    if (rest >= STLV_TLV_HDR_SIZE) {
        CHECK_EQ(stlv_build_put(&b, 3, big, rest - STLV_TLV_HDR_SIZE), STLV_OK);
    }
    CHECK_EQ(stlv_build_put(&b, 4, NULL, 0), STLV_ERR_NOSPC);
    CHECK(b.len <= STLV_MAX_PAYLOAD);
    CHECK_EQ(stlv_build_finish(&b, 0), STLV_FRAME_SIZE(b.len));

    /* 参数检查 */
    stlv_build_begin(&b, buf, sizeof(buf));
    CHECK_EQ(stlv_build_put(&b, 1, NULL, 1), STLV_ERR_PARAM);
    CHECK_EQ(stlv_build_put(&b, 1, big, (size_t)STLV_VALUE_MAX + 1), STLV_ERR_PARAM);
    CHECK_EQ(stlv_build_put_items(&b, NULL, 1), STLV_ERR_PARAM);
    CHECK_EQ(stlv_build_put_items(&b, NULL, 0), STLV_OK);

    /* 批量封包中途失败：已追加部分保留 */
    stlv_build_begin(&b, buf, STLV_FRAME_SIZE(2 * STLV_TLV_HDR_SIZE));
    const stlv_tlv_t items[] = {{1, 0, NULL}, {2, 0, NULL}, {3, 0, NULL}};
    CHECK_EQ(stlv_build_put_items(&b, items, 3), STLV_ERR_NOSPC);
    CHECK_EQ(stlv_validate(stlv_build_payload(&b), b.len), 2);
}

void suite_codec(void)
{
    RUN_TEST(test_crc_vectors);
    RUN_TEST(test_golden_frame);
    RUN_TEST(test_build_and_foreach);
    RUN_TEST(test_iter_edge_cases);
    RUN_TEST(test_build_limits);
}
