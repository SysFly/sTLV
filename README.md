# sTLV

面向 MCU 串口的轻量 TLV 帧协议库：C99、无动态内存、单个 `.c` + 两个头文件。

- 帧定界：`SOF + 长度 + 帧头校验 + CRC16`，坏帧后自动重同步，**被坏帧"吞掉"的后续好帧不会丢**
- 帧序号：自动检测丢帧并上报
- TLV 宽度：Type / Length 各 1 或 2 字节，编译期可配
- 自定义 Type 集 + 统一回调：可选类型表，自动校验 Value 长度
- 封包、拆包都能 `foreach` 遍历
- 资源：Cortex-M0 `-Os` 下代码约 1.7 KB，无 data/bss；RAM 只有一个 `stlv_t`（默认 344 B，约等于 `STLV_MAX_PAYLOAD + 88`）。请静态分配，不要放在栈上
- 要求 C99 编译器（GCC、Clang、IAR、ARMCC6、Keil MDK 的 C99 模式）

## 帧格式

```
+------+------+-----+---------+-----+----------------------+---------+
| SOF0 | SOF1 | SEQ | LEN(2B) | HCS | PAYLOAD (LEN bytes)  | CRC(2B) |
+------+------+-----+---------+-----+----------------------+---------+
  0xA5   0x5A  0-255   小端     CRC8    TLV TLV TLV ...       小端

HCS     = CRC-8  (poly 0x07, init 0x00)   覆盖 SEQ..LEN
CRC     = CRC-16/CCITT-FALSE              覆盖 SEQ..PAYLOAD
TLV     = TYPE (1/2B) | LEN (1/2B) | VALUE      多字节字段均为小端
```

固定开销 8 字节。HCS 能让接收端在帧头损坏时立即放弃，不会因为错误的 LEN 白等一大段数据。

## 配置

在 `stlv_config.h` 中，可以用 `-D` 覆盖，或者用 `-DSTLV_CONFIG_FILE='"my_cfg.h"'` 指定自己的配置头：

| 宏 | 默认 | 说明 |
|---|---|---|
| `STLV_TYPE_SIZE` | 1 | Type 字节数（1/2） |
| `STLV_LEN_SIZE` | 1 | TLV Length 字节数（1/2） |
| `STLV_MAX_PAYLOAD` | 256 | 单帧最大 payload，决定接收缓冲区大小 |
| `STLV_SOF0` / `STLV_SOF1` | 0xA5 / 0x5A | 帧起始标志 |
| `STLV_CRC_TABLE` | 0 | 1：CRC16 查表（多占 512 B Flash，更快） |
| `STLV_CONST_ROM` | 空 | 查表的存储修饰，哈佛结构 MCU 上可用来把表放进 Flash |
| `STLV_SEQ_GAP_MAX` | 127 | 序号前跳不超过该值算丢帧，超过算序号跳变 |

**通信双方的配置必须一致。** 库本身和调用它的代码也必须用同一套配置编译。`stlv_init()` 会比对配置指纹，不一致时返回 `STLV_ERR_CONFIG`。用 CMake 时，通过 `-DSTLV_DEFINITIONS="STLV_LEN_SIZE=2;STLV_MAX_PAYLOAD=512"` 设置，配置会自动传给所有链接 `stlv` 的目标。

## 快速上手

```c
#include "stlv.h"

enum { MSG_PING = 1, MSG_TEMP = 2, MSG_NAME = 3 };   /* 自定义 Type 集 */

static const stlv_type_desc_t types[] = {            /* 可选：类型表 + 长度约束 */
    {MSG_PING, 0, 0},
    {MSG_TEMP, 2, 2},
    {MSG_NAME, 1, 16},
};

static int uart_write(const uint8_t *d, size_t n, void *user)
{
    return HAL_UART_Transmit(&huart1, (uint8_t *)d, n, 100) == HAL_OK ? 0 : -1;
}

static void on_msg(stlv_t *ctx, const stlv_tlv_t *tlv, void *user)   /* 统一回调 */
{
    switch (tlv->type) {
    case MSG_PING: stlv_send_tlv(ctx, MSG_PING, NULL, 0); break;     /* 回调里可以发送 */
    case MSG_TEMP: set_temp((int16_t)stlv_rd_u16(tlv->value));  break;
    }
}

static stlv_t link;

void app_init(void)
{
    const stlv_cfg_t cfg = {
        .write = uart_write, .on_tlv = on_msg, .on_event = NULL,
        .types = types, .type_count = 3, .user = NULL,
    };
    stlv_init(&link, &cfg);
}
```

### 封包

```c
uint8_t buf[STLV_FRAME_SIZE(64)];
stlv_builder_t b;
stlv_build_begin(&b, buf, sizeof(buf));
stlv_build_put_u16(&b, MSG_TEMP, 235);
stlv_build_put(&b, MSG_NAME, "pump", 4);

const stlv_tlv_t items[] = {{MSG_PING, 0, NULL}, {MSG_NAME, 2, (const uint8_t *)"ok"}};
stlv_build_put_items(&b, items, 2);          /* 数组批量封包 */

stlv_iter_t it;
STLV_BUILD_FOREACH(it, &b) { /* 遍历已封入的 TLV */ }

stlv_send(&link, &b);                        /* 自动填 SEQ 和 CRC 并写出 */
```

`put` 失败（返回 `STLV_ERR_NOSPC`）时构建器保持不变，所以可以"装满就发"：先 `stlv_send` 当前帧，再 `stlv_build_begin` 继续往新帧里放。

只发一个 TLV 时用 `stlv_send_tlv()`，它分段写出，不需要帧缓冲区。

### 拆包

```c
/* 主循环：从中断/DMA 环形缓冲区取字节喂入，回调在这里同步执行 */
void app_poll(void)
{
    uint8_t chunk[32];
    size_t n = ringbuf_read(&uart_rx_rb, chunk, sizeof(chunk));
    if (n) {
        stlv_rx_feed(&link, chunk, n);
        last_rx_tick = now();
    } else if (now() - last_rx_tick > IDLE_TIMEOUT) {
        stlv_rx_flush(&link);                /* 空闲超时：放弃半帧 */
    }
}
```

不经过实例、直接遍历任意 TLV 序列：

```c
stlv_iter_t it;
STLV_FOREACH(it, payload, len) {
    use(it.tlv.type, it.tlv.value, it.tlv.len);
}
if (it.err != STLV_OK) { /* 结构损坏 */ }
```

### 事件与统计

`on_event` 可选，事件有 `FRAME_BEGIN/END`、`SEQ_GAP`（`info->lost` 为丢失帧数）、`SEQ_JUMP`、`ERR_HEADER`、`ERR_CRC`、`ERR_FORMAT`、`TLV_UNKNOWN`、`TLV_BAD_LEN`。`stlv_stats()` 返回累计计数。

配置了类型表后，未知 Type 或长度不合法的 TLV 会被**单独跳过**并上报事件，同一帧的其他 TLV 照常分发；未配置类型表时所有 TLV 都会分发。

## 可靠性语义

- **检测**：HCS + CRC16。CRC16 对随机损坏的漏检率约 1/65536；如果业务要求更高，请在应用层再加一道校验。
- **重同步**：任何校验失败只丢 1 字节然后重新找 SOF。即使损坏的帧头声称很长，把后面的好帧都"吃进去"了，CRC 失败后这些好帧也会从缓冲区里被重新解析出来。CRC 碰巧通过但 TLV 结构损坏的帧，同样当作假帧处理。
- **丢帧检测**：8 位序号，每帧加 1。前跳不超过 `STLV_SEQ_GAP_MAX`（默认 127）时上报 `SEQ_GAP` 并计入 `rx_lost`；更大的跳变（重复帧、回退、对端重启）上报 `SEQ_JUMP`，不计入丢帧。首帧不判断。库**不做重传，也不去重**，重复帧照常分发。
- **写失败**：`write` 回调返回失败时，发送序号照样递增，所以接收端会把它识别为丢了 1 帧。
- **空闲 flush**：最后一帧被截断时，解析器会一直等待剩余字节。请在串口空闲超时后调用 `stlv_rx_flush()`，它会放弃这个半帧，并解析缓冲区里其后已收齐的帧。
- **线程模型**：一个实例不可重入。回调里可以调用 `stlv_send*`，也可以调用 `stlv_init()` 复位实例：当前帧剩余的 TLV 和已缓冲的数据都会被丢弃，之后喂入的数据正常处理。回调里不能调用 `stlv_rx_feed/flush`（会返回 `STLV_ERR_BUSY`）。中断里只把字节放进环形缓冲区，`stlv_rx_feed` 放到主循环或任务里调用。
- **处理耗时**：接收缓冲区用读指针实现，重同步时每丢 1 字节的代价是 O(1)，没有整块搬移。但每个帧头校验通过的候选帧都要做一次 CRC，所以在噪声严重时，最坏耗时约为"候选帧数 × 帧长"。`STLV_MAX_PAYLOAD` 设得很大时，要注意单次 `feed` 的耗时上限。
- **不要嵌套帧**：payload 里不要原样携带另一个 sTLV 帧（例如转发或记录原始帧）。外层帧一旦损坏，内层帧会被当作独立的帧收下。确实需要时，应先对其做转义或编码。
- 回调里拿到的 `tlv->value` 指向内部缓冲区，回调返回后失效。

## 构建与测试

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

测试会按 6 组配置分别编译（T1L1/256、T1L2/1024+查表、T2L1/64、T2L2/600+查表、T1L2/65535+查表、T1L1/32），默认开启 ASan/UBSan（`-DSTLV_SANITIZE=OFF` 关闭）。

- `test/test_codec.c`：CRC 标准向量、独立实现生成的 golden 帧、封包/遍历、边界条件
- `test/test_rx.c`：分块/逐字节喂入、噪声、payload 中含 SOF、各类错误恢复、flush、序号回绕/丢帧/跳变、类型表、发送路径、回调内回发、回调内复位、写到一半失败、长串 SOF 字节
- `test/test_fault.c`：故障注入。信道会丢帧、丢字节、多字节、翻转比特、截断、插入噪声和伪造帧头，并在随机空闲点调用 flush。断言以"预言机"为准：在最终字节流上推出哪些帧原样存在，接收端必须恰好交付这些帧，内容逐字节一致，丢帧统计和丢弃字节数也要完全对上

长时间浸泡：

```sh
STLV_FAULT_SEED=0xdeadbeef STLV_FAULT_FRAMES=200000 ./build/stlv_test_t1l1
```

生成 API 文档：`doxygen Doxyfile`，输出在 `build/docs/html`。

## 移植清单

1. 把 `include/` 和 `src/stlv.c` 加入工程（只依赖 `<string.h>`、`<stdint.h>`、`<stdbool.h>`）
2. 按需设置 `STLV_MAX_PAYLOAD` 等配置
3. 实现 `write` 回调（阻塞发送，或写入发送环形缓冲区）
4. 在串口中断里收字节存入环形缓冲区，主循环里 `stlv_rx_feed()`，空闲超时后 `stlv_rx_flush()`
