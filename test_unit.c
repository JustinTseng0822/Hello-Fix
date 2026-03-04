/*
 * test_unit.c — Unit tests for calculate_checksum()
 *
 * 測試策略：
 *   - 不依賴任何第三方框架
 *   - 以自訂 PASS / FAIL 巨集輸出結果
 *   - 任何一個測試失敗，最終回傳非零結束碼，讓 make 偵測到錯誤
 *
 * 編譯方式（由 Makefile test target 呼叫
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * calculate_checksum() 的原始實作（直接複製自 fix_client.c，
 * 避免 link 時把 main() 也拉進來）
 * ----------------------------------------------------------------------- */
static unsigned int calculate_checksum(const char *msg, size_t len)
{
    unsigned int sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += (unsigned char)msg[i];

    return sum % 256;
}

/* -----------------------------------------------------------------------
 * 測試輔助巨集
 * ----------------------------------------------------------------------- */
static int g_pass_count = 0;
static int g_fail_count = 0;

#define CHECK_EQUAL(desc, expected, actual)                              \
    do {                                                                 \
        unsigned int _exp = (unsigned int)(expected);                   \
        unsigned int _act = (unsigned int)(actual);                     \
        if (_exp == _act) {                                              \
            printf("[PASS] %s\n", (desc));                              \
            g_pass_count++;                                              \
        } else {                                                         \
            printf("[FAIL] %s — expected %u, got %u\n",                 \
                   (desc), _exp, _act);                                  \
            g_fail_count++;                                              \
        }                                                                \
    } while (0)

/* -----------------------------------------------------------------------
 * 測試案例
 * ----------------------------------------------------------------------- */

/*
 * TC-01: 正常 FIX 訊息 checksum
 *
 * 這是 fix_server.c 實際傳送的那筆訊息（不含 "10=233\x01" 欄位本身）。
 * FIX checksum 的計算範圍是從訊息開頭到 "10=" 之前的所有位元組。
 * 預期值：233（與 server 訊息中的 10=233 相符）。
 */
static void test_fix_message_checksum(void)
{
    /*
     * fix_server.c 中的完整 fix_msg：
     *   "8=FIX.4.2\x01" "9=51\x01" "35=0\x01" "49=200201\x01"
     *   "56=TWSE\x01"   "34=593\x01" "52=20090803-13:37:47\x01"
     *   "10=233\x01"
     *
     * checksum 計算範圍 = "10=" 之前的所有位元組（共 66 bytes）。
     */
    const char prefix[] =
        "8=FIX.4.2\x01"
        "9=51\x01"
        "35=0\x01"
        "49=200201\x01"
        "56=TWSE\x01"
        "34=593\x01"
        "52=20090803-13:37:47\x01";

    /* sizeof 包含結尾 '\0'，減 1 取實際位元組數 */
    size_t len = sizeof(prefix) - 1;

    unsigned int result = calculate_checksum(prefix, len);

    CHECK_EQUAL("TC-01: FIX message checksum == 233", 233, result);
}

/*
 * TC-02: 空訊息（len = 0）的 checksum 應為 0
 *
 * 迴圈一次都不執行，sum 保持 0，0 % 256 = 0。
 */
static void test_empty_message(void)
{
    unsigned int result = calculate_checksum("", 0);

    CHECK_EQUAL("TC-02: empty message checksum == 0", 0, result);
}

/*
 * TC-03: 單一 byte 'A' (ASCII 65)
 *
 * sum = 65，65 % 256 = 65。
 */
static void test_single_byte_A(void)
{
    const char msg[] = "A";    /* 'A' = 0x41 = 65 */
    unsigned int result = calculate_checksum(msg, 1);

    CHECK_EQUAL("TC-03: single byte 'A' checksum == 65", 65, result);
}

/*
 * TC-04: 單一 byte 0xFF（有號 char 下為 -1）
 *
 * 實作以 (unsigned char) 轉型，因此 0xFF → 255。
 * 255 % 256 = 255。
 * 此測試驗證「有負值 char 時轉型正確」。
 */
static void test_single_byte_0xFF(void)
{
    /* 使用 char 陣列存放 0xFF；
     * 在有號 char 平台上，(char)0xFF == -1，
     * 函式必須以 (unsigned char) 轉型才能得到 255。 */
    const char msg[] = { (char)0xFF, '\0' };
    unsigned int result = calculate_checksum(msg, 1);

    CHECK_EQUAL("TC-04: single byte 0xFF checksum == 255", 255, result);
}

/*
 * TC-05: 兩個 byte：0xFF + 0x01 → overflow wrap-around
 *
 * (unsigned char)0xFF = 255
 * (unsigned char)0x01 = 1
 * sum = 256，256 % 256 = 0。
 * 驗證模 256 的 wrap-around 行為。
 */
static void test_wrap_around(void)
{
    const char msg[] = { (char)0xFF, (char)0x01 };
    unsigned int result = calculate_checksum(msg, 2);

    CHECK_EQUAL("TC-05: 0xFF+0x01 wrap-around checksum == 0", 0, result);
}

/*
 * TC-06: 多個負值 char 的混合訊息
 *
 * 選用 0x80（有號 char = -128）與 0x7F（有號 char = 127）：
 * (unsigned char)0x80 = 128
 * (unsigned char)0x7F = 127
 * sum = 255，255 % 256 = 255。
 */
static void test_negative_chars_mixed(void)
{
    const char msg[] = { (char)0x80, (char)0x7F };
    unsigned int result = calculate_checksum(msg, 2);

    CHECK_EQUAL("TC-06: 0x80+0x7F mixed negative checksum == 255", 255, result);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("========================================\n");
    printf(" Unit Tests: calculate_checksum()\n");
    printf("========================================\n");

    test_fix_message_checksum();
    test_empty_message();
    test_single_byte_A();
    test_single_byte_0xFF();
    test_wrap_around();
    test_negative_chars_mixed();

    printf("----------------------------------------\n");
    printf("Results: %d passed, %d failed\n", g_pass_count, g_fail_count);
    printf("========================================\n");

    /* 有任何失敗則回傳 1，讓 make 視為錯誤 */
    return (g_fail_count > 0) ? 1 : 0;
}
