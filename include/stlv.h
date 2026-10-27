/**
 * @file stlv.h
 * @brief sTLV：面向 MCU 串口的轻量 TLV 帧协议库。
 *
 * 帧格式（多字节字段均为小端）：
 * @verbatim
 *  +------+------+-----+---------+-----+----------------------+---------+
 *  | SOF0 | SOF1 | CTL | LEN(2B) | HCS | PAYLOAD (LEN bytes)  | CRC(2B) |
 *  +------+------+-----+---------+-----+----------------------+---------+
 *    0xA5   0x5A          LE       CRC8   TLV TLV TLV ...        CRC16
 *
 *  CTL   = bit7 SYN | bit0-6 SEQ（发送端初始化后的前 STLV_SYN_FRAMES 帧置 SYN）
 *  HCS   = CRC-8 (poly 0x07, init 0x00) over CTL..LEN    (3 bytes)
 *  CRC   = CRC-16/CCITT-FALSE           over CTL..PAYLOAD
 *  TLV   = TYPE (STLV_TYPE_SIZE B) | LEN (STLV_LEN_SIZE B) | VALUE (LEN B)
 * @endverbatim
 *
 * 不使用动态内存；单个 stlv_t 实例不可重入，同一实例的接收和发送需在同一上下文或自行加锁。
 * 要求 C99 编译器。
 */
#ifndef STLV_H
#define STLV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stlv_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 帧头长度：SOF0 SOF1 CTL LEN(2) HCS。 */
#define STLV_HDR_SIZE 6u
/** @brief 帧尾长度：CRC16。 */
#define STLV_CRC_SIZE 2u
/** @brief 单帧固定开销字节数。 */
#define STLV_FRAME_OVERHEAD (STLV_HDR_SIZE + STLV_CRC_SIZE)
/** @brief CTL 字节中的序号掩码（7 位序号）。 */
#define STLV_SEQ_MASK 0x7Fu
/** @brief CTL 字节中的 SYN 标志：发送端刚启动。 */
#define STLV_FLAG_SYN 0x80u
/** @brief 单个 TLV 头（Type+Length）字节数。 */
#define STLV_TLV_HDR_SIZE (STLV_TYPE_SIZE + STLV_LEN_SIZE)
/** @brief 承载 @p payload 字节 payload 的完整帧所需缓冲区大小。 */
#define STLV_FRAME_SIZE(payload) ((size_t)(payload) + STLV_FRAME_OVERHEAD)

#if STLV_MAX_PAYLOAD > SIZE_MAX - 8
#error "STLV_MAX_PAYLOAD too large for this platform's size_t"
#endif
/** @brief 最大帧长度。 */
#define STLV_MAX_FRAME STLV_FRAME_SIZE(STLV_MAX_PAYLOAD)

#if STLV_TYPE_SIZE == 1
typedef uint8_t stlv_type_t; /**< Type 字段类型。 */
#else
typedef uint16_t stlv_type_t;
#endif

#if STLV_LEN_SIZE == 1
typedef uint8_t stlv_len_t; /**< TLV Length 字段类型。 */
#define STLV_VALUE_MAX 0xFFu /**< 单个 Value 最大长度。 */
#else
typedef uint16_t stlv_len_t;
#define STLV_VALUE_MAX 0xFFFFu
#endif

/** @brief 返回码。 */
typedef enum {
    STLV_OK = 0,          /**< 成功。 */
    STLV_ERR_PARAM = -1,  /**< 参数非法。 */
    STLV_ERR_NOSPC = -2,  /**< 缓冲区或帧长度不足。 */
    STLV_ERR_IO = -3,     /**< 写串口回调返回失败。 */
    STLV_ERR_BUSY = -4,   /**< 在接收回调中重入 stlv_rx_feed()。 */
    STLV_ERR_FORMAT = -5, /**< TLV 结构损坏。 */
    STLV_ERR_CONFIG = -6  /**< 库与调用方的编译配置不一致。 */
} stlv_err_t;

/** @brief 一个 TLV 元素；遍历/接收时 value 指向原缓冲区，接收回调返回后失效。 */
typedef struct {
    stlv_type_t type;     /**< 类型。 */
    stlv_len_t len;       /**< Value 长度。 */
    const uint8_t *value; /**< Value 数据；封包时 len 为 0 可为 NULL。 */
} stlv_tlv_t;

/* ------------------------------------------------------------------------ */
/** @name TLV 遍历（拆包）
 *  @{ */

/** @brief TLV 迭代器，配合 STLV_FOREACH 使用。 */
typedef struct {
    const uint8_t *pos; /**< 下一个 TLV 位置。 */
    const uint8_t *end; /**< payload 结束位置。 */
    stlv_tlv_t tlv;     /**< 当前 TLV。 */
    int err;            /**< 遍历结束后为 STLV_OK 或 STLV_ERR_FORMAT。 */
} stlv_iter_t;

/**
 * @brief 初始化迭代器。
 * @param it      迭代器。
 * @param payload TLV 序列起始地址。
 * @param len     TLV 序列字节数。
 */
void stlv_iter_init(stlv_iter_t *it, const void *payload, size_t len);

/**
 * @brief 取下一个 TLV 到 it->tlv。
 * @return true 取到；false 已结束或结构损坏（查看 it->err）。
 */
bool stlv_iter_next(stlv_iter_t *it);

/**
 * @brief 遍历一段 TLV 序列。
 *
 * @code
 * stlv_iter_t it;
 * STLV_FOREACH(it, payload, len) {
 *     handle(it.tlv.type, it.tlv.value, it.tlv.len);
 * }
 * if (it.err != STLV_OK) { ... }
 * @endcode
 */
#define STLV_FOREACH(it, payload, len) \
    for (stlv_iter_init(&(it), (payload), (len)); stlv_iter_next(&(it));)

/**
 * @brief 校验 TLV 序列结构。
 * @return TLV 个数（>=0），结构损坏返回 STLV_ERR_FORMAT。
 */
int stlv_validate(const void *payload, size_t len);

/** @} */

/* ------------------------------------------------------------------------ */
/** @name 帧构建（封包）
 *  @{ */

/** @brief 帧构建器，直接在用户缓冲区内组帧，无额外拷贝。 */
typedef struct {
    uint8_t *buf; /**< 帧缓冲区（含帧头和 CRC 空间）。 */
    size_t cap;   /**< 缓冲区容量。 */
    size_t len;   /**< 当前 payload 长度。 */
} stlv_builder_t;

/**
 * @brief 开始组帧。
 * @param b   构建器。
 * @param buf 帧缓冲区，建议大小 STLV_FRAME_SIZE(预计 payload)。
 * @param cap 缓冲区字节数，至少 STLV_FRAME_OVERHEAD。
 * @return STLV_OK；参数非法返回 STLV_ERR_PARAM，此后 put/finish 均失败。
 */
int stlv_build_begin(stlv_builder_t *b, void *buf, size_t cap);

/**
 * @brief 追加一个 TLV；失败时构建器不变，可先发送已有内容再继续。
 * @param b     构建器。
 * @param type  类型。
 * @param value Value 数据，len 为 0 时可为 NULL。
 * @param len   Value 长度，不超过 STLV_VALUE_MAX。
 * @return STLV_OK；空间不足 STLV_ERR_NOSPC；参数非法 STLV_ERR_PARAM。
 */
int stlv_build_put(stlv_builder_t *b, stlv_type_t type, const void *value, size_t len);

/**
 * @brief 依次追加 TLV 数组中的每个元素（foreach 封包）。
 * @return STLV_OK 或首个失败的错误码；失败前已追加的元素保留。
 */
int stlv_build_put_items(stlv_builder_t *b, const stlv_tlv_t *items, size_t count);

/** @brief 追加 1 字节 Value。 */
int stlv_build_put_u8(stlv_builder_t *b, stlv_type_t type, uint8_t v);
/** @brief 追加 2 字节小端 Value。 */
int stlv_build_put_u16(stlv_builder_t *b, stlv_type_t type, uint16_t v);
/** @brief 追加 4 字节小端 Value。 */
int stlv_build_put_u32(stlv_builder_t *b, stlv_type_t type, uint32_t v);

/** @brief 当前 payload 起始地址，构建器无效时返回 NULL。 */
static inline const uint8_t *stlv_build_payload(const stlv_builder_t *b)
{
    return b->cap != 0 ? b->buf + STLV_HDR_SIZE : NULL;
}

/** @brief 遍历构建器中已封入的 TLV。 */
#define STLV_BUILD_FOREACH(it, b) STLV_FOREACH(it, stlv_build_payload(b), (b)->len)

/**
 * @brief 写入帧头和 CRC，完成组帧；之后仍可继续 put 并再次 finish。
 *
 * 通过 stlv_send() 发送时由实例自动填写 CTL，无需调用本函数。
 * @param b   构建器。
 * @param ctl CTL 字节：序号（0..127）| 可选 STLV_FLAG_SYN。
 * @return 完整帧长度（帧从 b->buf 开始）；构建器无效时返回 0。
 */
size_t stlv_build_finish(stlv_builder_t *b, uint8_t ctl);

/** @} */

/* ------------------------------------------------------------------------ */
/** @name 协议实例（收发）
 *  @{ */

/** @brief 协议实例，见 struct stlv。 */
typedef struct stlv stlv_t;

/** @brief 接收事件，用于诊断和统计。 */
typedef enum {
    STLV_EV_FRAME_BEGIN,   /**< 收到有效帧，即将分发 TLV。 */
    STLV_EV_FRAME_END,     /**< 该帧 TLV 分发完毕。 */
    STLV_EV_SEQ_GAP,       /**< 同一会话内序号前跳不超过 STLV_SEQ_GAP_MAX，info->lost 为丢失的帧数。 */
    STLV_EV_ERR_HEADER,    /**< 帧头校验失败或 LEN 超过 STLV_MAX_PAYLOAD。 */
    STLV_EV_ERR_CRC,       /**< 帧 CRC 校验失败。 */
    STLV_EV_ERR_FORMAT,    /**< CRC 正确但 TLV 结构损坏，视为假帧丢弃并重新同步。 */
    STLV_EV_TLV_UNKNOWN,   /**< Type 不在类型表中，该 TLV 跳过。 */
    STLV_EV_TLV_BAD_LEN,   /**< Value 长度不符合类型表，该 TLV 跳过。 */
    STLV_EV_SEQ_JUMP,      /**< 序号对不上（重复帧、回退或前跳过大），重新同步，不计入丢帧。 */
    STLV_EV_PEER_RESET     /**< 收到新会话的 SYN 帧，判定对端已重启，重新同步，不计入丢帧。 */
} stlv_event_t;

/** @brief 事件附带信息，按事件类型取用相关字段。 */
typedef struct {
    uint8_t seq;          /**< 7 位帧序号；ERR_HEADER/ERR_CRC 时为未经校验的原始 CTL 字节。 */
    uint8_t lost;         /**< 丢失帧数（SEQ_GAP），其余事件为 0。 */
    uint16_t payload_len; /**< payload 长度；ERR_HEADER/ERR_CRC 时为未经校验的原始值。 */
    const stlv_tlv_t *tlv;/**< 相关 TLV（TLV_*），其余事件为 NULL。 */
} stlv_event_info_t;

/** @brief 统一消息回调：每个合法 TLV 调用一次。 */
typedef void (*stlv_tlv_cb)(stlv_t *ctx, const stlv_tlv_t *tlv, void *user);

/** @brief 事件回调（可选）。 */
typedef void (*stlv_event_cb)(stlv_t *ctx, stlv_event_t ev, const stlv_event_info_t *info,
                              void *user);

/**
 * @brief 串口写回调。
 * @return 0 表示全部写出，非 0 表示失败。
 */
typedef int (*stlv_write_fn)(const uint8_t *data, size_t len, void *user);

/** @brief 类型表项：声明一个合法 Type 及其 Value 长度范围。 */
typedef struct {
    stlv_type_t type;    /**< 类型。 */
    stlv_len_t min_len;  /**< 最小 Value 长度。 */
    stlv_len_t max_len;  /**< 最大 Value 长度。 */
} stlv_type_desc_t;

/** @brief 实例配置。 */
typedef struct {
    stlv_write_fn write;            /**< 串口写，仅接收时可为 NULL。 */
    stlv_tlv_cb on_tlv;             /**< 统一消息回调，可为 NULL。 */
    stlv_event_cb on_event;         /**< 事件回调，可为 NULL。 */
    const stlv_type_desc_t *types;  /**< 类型表，NULL 表示接受任意 Type。 */
    size_t type_count;              /**< 类型表项数。 */
    void *user;                     /**< 透传给所有回调的用户指针。 */
} stlv_cfg_t;

/** @brief 统计计数。 */
typedef struct {
    uint32_t rx_frames;       /**< 收到的有效帧。 */
    uint32_t rx_tlvs;         /**< 分发的 TLV。 */
    uint32_t rx_lost;         /**< 按序号推断丢失的帧。 */
    uint32_t rx_seq_jump;     /**< 序号跳变次数（重复帧、回退或前跳过大）。 */
    uint32_t rx_peer_reset;   /**< 检测到对端重启的次数。 */
    uint32_t rx_discarded;    /**< 重同步时丢弃的字节。 */
    uint32_t err_header;      /**< 帧头错误次数。 */
    uint32_t err_crc;         /**< CRC 错误次数。 */
    uint32_t err_format;      /**< TLV 结构错误帧数。 */
    uint32_t tlv_unknown;     /**< 未知 Type 的 TLV。 */
    uint32_t tlv_bad_len;     /**< 长度不合法的 TLV。 */
    uint32_t tx_frames;       /**< 发出的帧。 */
} stlv_stats_t;

/**
 * @brief 协议实例，字段视为私有，仅通过 API 访问。
 *
 * 大小约 STLV_MAX_PAYLOAD + 80 字节，建议静态分配，不要放在栈上。
 */
struct stlv {
    stlv_cfg_t cfg;                 /**< 配置副本。 */
    stlv_stats_t stats;             /**< 统计计数。 */
    uint32_t rx_head;               /**< 接收缓冲区读位置。 */
    uint32_t rx_len;                /**< 接收缓冲区有效字节数。 */
    uint8_t rx_expect_seq;          /**< 期望的下一帧序号。 */
    uint8_t rx_seq_valid;           /**< 是否已收到过有效帧。 */
    uint8_t rx_last_syn;            /**< 上一有效帧是否带 SYN。 */
    uint8_t tx_syn_left;            /**< 还要带 SYN 发送的帧数。 */
    uint8_t rx_busy;                /**< 防重入标志。 */
    uint8_t tx_seq;                 /**< 下一帧发送序号。 */
    uint8_t rx_buf[STLV_MAX_FRAME]; /**< 接收缓冲区。 */
};

/** @brief 编译配置指纹，用于检测库与调用方配置不一致。 */
#define STLV_ABI                                                                \
    ((uint32_t)STLV_MAX_PAYLOAD | ((uint32_t)STLV_TYPE_SIZE << 16) |            \
     ((uint32_t)STLV_LEN_SIZE << 18) | ((uint32_t)(STLV_SEQ_GAP_MAX & 0xFF) << 20) | \
     ((uint32_t)((STLV_SOF0 ^ STLV_SOF1) & 0x0F) << 28))

/**
 * @brief 初始化实例（也可用于复位，包括在回调中调用）。
 * @param ctx 实例。
 * @param cfg 配置，内容会被复制。
 * @return STLV_OK；参数非法 STLV_ERR_PARAM；库与调用方编译配置不一致 STLV_ERR_CONFIG。
 */
#define stlv_init(ctx, cfg) stlv_init_abi((ctx), (cfg), STLV_ABI)

/** @brief stlv_init() 的实现，请勿直接调用。 */
int stlv_init_abi(stlv_t *ctx, const stlv_cfg_t *cfg, uint32_t abi);

/**
 * @brief 喂入串口收到的字节，可在主循环中按任意分块调用；回调在此函数内执行。
 * @return STLV_OK；参数非法 STLV_ERR_PARAM；在回调中重入返回 STLV_ERR_BUSY。
 */
int stlv_rx_feed(stlv_t *ctx, const void *data, size_t len);

/**
 * @brief 视为数据流已中断：解析缓冲区中所有已完整到达的帧，丢弃其余字节，清空缓冲区。
 *
 * 建议在串口空闲超时（例如若干字节时间无新数据）时调用，避免损坏的 LEN 导致后续帧一直
 * 等到下一批数据才被处理。序号跟踪保留。
 * @return STLV_OK；参数非法 STLV_ERR_PARAM；在回调中调用返回 STLV_ERR_BUSY。
 */
int stlv_rx_flush(stlv_t *ctx);

/**
 * @brief 用实例的发送序号完成组帧并写出。
 *
 * 只要调用了 write 回调，发送序号就自增（写失败也一样），这样接收端可以把
 * 写失败识别为丢帧。
 * @return STLV_OK、STLV_ERR_IO，构建器无效或未配置 write 时返回 STLV_ERR_PARAM。
 */
int stlv_send(stlv_t *ctx, stlv_builder_t *b);

/**
 * @brief 直接发送仅含一个 TLV 的帧，分 3 次调用 write 写出，无需帧缓冲区。
 * @return 同 stlv_send()；len 超过 STLV_VALUE_MAX 返回 STLV_ERR_PARAM，
 *         帧超过 STLV_MAX_PAYLOAD 返回 STLV_ERR_NOSPC。
 */
int stlv_send_tlv(stlv_t *ctx, stlv_type_t type, const void *value, size_t len);

/** @brief 读取统计计数，ctx 为 NULL 时返回 NULL。 */
const stlv_stats_t *stlv_stats(const stlv_t *ctx);

/** @} */

/* ------------------------------------------------------------------------ */
/** @name 工具函数
 *  @{ */

/** @brief CRC-16/CCITT-FALSE 增量计算，首次传入 0xFFFF。 */
uint16_t stlv_crc16(uint16_t crc, const void *data, size_t len);

/** @brief CRC-8 (poly 0x07) 增量计算，首次传入 0x00。 */
uint8_t stlv_crc8(uint8_t crc, const void *data, size_t len);

/** @brief 读取小端 uint16。 */
static inline uint16_t stlv_rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/** @brief 读取小端 uint32。 */
static inline uint32_t stlv_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* STLV_H */
