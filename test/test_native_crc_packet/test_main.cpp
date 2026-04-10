#include <unity.h>

#include "packet_builder.h"
#include "../../src/packet_builder.cpp"

void test_crc16_known_vector(void) {
    const uint8_t bytes[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16_ccitt_false(bytes, sizeof(bytes)));
}

void test_packet_layout_contains_port_seq_len_and_crc(void) {
    uint8_t out[32] = {0};
    const size_t n = build_packet(2, 7, 16, out, sizeof(out));

    TEST_ASSERT_EQUAL_UINT32(16, n);
    TEST_ASSERT_EQUAL_HEX8(0x51, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[1]);
    TEST_ASSERT_EQUAL_UINT8(2, out[2]);
    TEST_ASSERT_EQUAL_UINT8(16, out[7]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_crc16_known_vector);
    RUN_TEST(test_packet_layout_contains_port_seq_len_and_crc);
    return UNITY_END();
}
