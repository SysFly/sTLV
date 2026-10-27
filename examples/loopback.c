/*
 * sTLV 使用示例：两个实例通过内存"串口"互连。
 * 移植到 MCU 时，把 uart_write 换成 HAL 发送，把 rx 字节从串口中断/DMA 环形缓冲区
 * 取出后在主循环里调用 stlv_rx_feed()，并在串口空闲超时后调用 stlv_rx_flush()。
 */
#include <stdio.h>
#include <string.h>

#include "stlv.h"

/* 1. 自定义 Type 集 */
enum {
    MSG_PING = 0x01,
    MSG_TEMP = 0x02, /* int16，单位 0.1°C */
    MSG_NAME = 0x03, /* 1~16 字节字符串 */
};

static const stlv_type_desc_t msg_types[] = {
    {MSG_PING, 0, 0},
    {MSG_TEMP, 2, 2},
    {MSG_NAME, 1, 16},
};

/* 模拟串口线路 */
static uint8_t wire[256];
static size_t wire_len;

static int uart_write(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (wire_len + len > sizeof(wire)) {
        return -1;
    }
    memcpy(wire + wire_len, data, len);
    wire_len += len;
    return 0;
}

/* 2. 统一消息回调 */
static void on_msg(stlv_t *ctx, const stlv_tlv_t *tlv, void *user)
{
    (void)ctx;
    (void)user;
    switch (tlv->type) {
    case MSG_PING:
        printf("PING\n");
        break;
    case MSG_TEMP:
        printf("TEMP %.1f C\n", (int16_t)stlv_rd_u16(tlv->value) / 10.0);
        break;
    case MSG_NAME:
        printf("NAME %.*s\n", (int)tlv->len, (const char *)tlv->value);
        break;
    default:
        break;
    }
}

static void on_event(stlv_t *ctx, stlv_event_t ev, const stlv_event_info_t *info, void *user)
{
    (void)ctx;
    (void)user;
    if (ev == STLV_EV_SEQ_GAP) {
        printf("lost %u frame(s) before seq %u\n", info->lost, info->seq);
    } else if (ev == STLV_EV_PEER_RESET) {
        printf("peer restarted\n"); /* 在这里清理与对端相关的会话状态 */
    } else if (ev == STLV_EV_ERR_CRC) {
        printf("crc error\n");
    }
}

int main(void)
{
    stlv_t tx, rx;
    const stlv_cfg_t tx_cfg = {uart_write, NULL, NULL, NULL, 0, NULL};
    const stlv_cfg_t rx_cfg = {NULL, on_msg, on_event, msg_types, 3, NULL};
    stlv_init(&tx, &tx_cfg);
    stlv_init(&rx, &rx_cfg);

    /* 3. 封包：一帧多个 TLV */
    uint8_t buf[STLV_FRAME_SIZE(64)];
    stlv_builder_t b;
    stlv_build_begin(&b, buf, sizeof(buf));
    stlv_build_put(&b, MSG_PING, NULL, 0);
    stlv_build_put_u16(&b, MSG_TEMP, (uint16_t)(int16_t)-123);
    stlv_build_put(&b, MSG_NAME, "sensor-1", 8);

    stlv_iter_t it;
    STLV_BUILD_FOREACH(it, &b) {
        printf("packed type=0x%02X len=%u\n", (unsigned)it.tlv.type, (unsigned)it.tlv.len);
    }
    stlv_send(&tx, &b);

    /* 单 TLV 快捷发送 */
    stlv_send_tlv(&tx, MSG_NAME, "pump", 4);

    /* 模拟发送端重启：重新 init 后序号归零并带 SYN，接收端会上报 PEER_RESET */
    stlv_init(&tx, &tx_cfg);
    stlv_send_tlv(&tx, MSG_PING, NULL, 0);

    /* 4. 拆包：任意分块喂入，回调里逐 TLV 处理 */
    for (size_t i = 0; i < wire_len; i += 5) {
        size_t n = wire_len - i < 5 ? wire_len - i : 5;
        stlv_rx_feed(&rx, wire + i, n);
    }
    stlv_rx_flush(&rx); /* 串口空闲 */

    const stlv_stats_t *s = stlv_stats(&rx);
    printf("rx frames=%u tlvs=%u crc_err=%u peer_reset=%u\n", (unsigned)s->rx_frames,
           (unsigned)s->rx_tlvs, (unsigned)s->err_crc, (unsigned)s->rx_peer_reset);
    return 0;
}
