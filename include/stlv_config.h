/**
 * @file stlv_config.h
 * @brief sTLV 编译期配置。
 *
 * 所有选项都可以通过编译器 -D 覆盖，或者定义 STLV_CONFIG_FILE 指向用户配置头文件，
 * 例如 `-DSTLV_CONFIG_FILE=\"my_stlv_cfg.h\"`。
 */
#ifndef STLV_CONFIG_H
#define STLV_CONFIG_H

#ifdef STLV_CONFIG_FILE
#include STLV_CONFIG_FILE
#endif

/** @brief TLV 中 Type 字段字节数，取 1 或 2。 */
#ifndef STLV_TYPE_SIZE
#define STLV_TYPE_SIZE 1
#endif

/** @brief TLV 中 Length 字段字节数，取 1 或 2。 */
#ifndef STLV_LEN_SIZE
#define STLV_LEN_SIZE 1
#endif

/** @brief 单帧最大 payload 字节数（1..65535），决定接收缓冲区大小。 */
#ifndef STLV_MAX_PAYLOAD
#define STLV_MAX_PAYLOAD 256
#endif

/** @brief 帧起始标志第 1 字节。 */
#ifndef STLV_SOF0
#define STLV_SOF0 0xA5u
#endif

/** @brief 帧起始标志第 2 字节。 */
#ifndef STLV_SOF1
#define STLV_SOF1 0x5Au
#endif

/** @brief 为 1 时 CRC16 使用 512 字节查表实现（更快），为 0 时使用逐位计算（省 Flash）。 */
#ifndef STLV_CRC_TABLE
#define STLV_CRC_TABLE 0
#endif

/**
 * @brief CRC 查表的存储修饰。
 *
 * 在 AVR 等哈佛结构 MCU 上，`static const` 数据默认会被拷贝到 RAM，可在这里加上编译器的
 * Flash 属性。注意：avr-gcc 的 PROGMEM 需要用专门的读函数访问，不能直接在这里使用。
 */
#ifndef STLV_CONST_ROM
#define STLV_CONST_ROM
#endif

/**
 * @brief 序号差值不超过该值时，视为中间丢了帧（STLV_EV_SEQ_GAP）；
 *        超过时视为重复帧或对端重启（STLV_EV_SEQ_JUMP），不计入丢帧数。取值 1..254。
 */
#ifndef STLV_SEQ_GAP_MAX
#define STLV_SEQ_GAP_MAX 127
#endif

#if STLV_TYPE_SIZE != 1 && STLV_TYPE_SIZE != 2
#error "STLV_TYPE_SIZE must be 1 or 2"
#endif
#if STLV_LEN_SIZE != 1 && STLV_LEN_SIZE != 2
#error "STLV_LEN_SIZE must be 1 or 2"
#endif
#if STLV_MAX_PAYLOAD < 1 || STLV_MAX_PAYLOAD > 65535
#error "STLV_MAX_PAYLOAD must be in 1..65535"
#endif
#if STLV_SEQ_GAP_MAX < 1 || STLV_SEQ_GAP_MAX > 254
#error "STLV_SEQ_GAP_MAX must be in 1..254"
#endif

#endif /* STLV_CONFIG_H */
