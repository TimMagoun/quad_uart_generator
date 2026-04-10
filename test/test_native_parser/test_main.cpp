#include <unity.h>

#include <string>

#include "command_parser.h"
#include "quad_uart_controller.h"

class FakeWriter : public UartTxWriter {
public:
    bool begin_port(uint8_t, const PortConfig&) override { return true; }
    int available_for_write(uint8_t) override { return 0; }
    size_t write_byte(uint8_t, uint8_t) override { return 0; }
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) < haystack.size();
}

void test_parse_format_8e2(void) {
    ParsedFormat f{};
    TEST_ASSERT_TRUE(parse_format_token("8E2", &f));
    TEST_ASSERT_EQUAL_UINT8(8, f.data_bits);
    TEST_ASSERT_EQUAL_UINT8(2, f.stop_bits);
    TEST_ASSERT_EQUAL_UINT8('E', static_cast<uint8_t>(f.parity));
}

void test_parse_format_reject_invalid(void) {
    ParsedFormat f{};
    TEST_ASSERT_FALSE(parse_format_token("9N1", &f));
    TEST_ASSERT_FALSE(parse_format_token("8X1", &f));
    TEST_ASSERT_FALSE(parse_format_token("8N3", &f));
}

void test_cfg_rejected_when_any_port_enabled(void) {
    FakeWriter writer;
    QuadUartController c(&writer);

    std::string r = c.handle_command("PORT 0 ENABLE", 0);
    TEST_ASSERT_EQUAL_STRING("OK", r.c_str());

    r = c.handle_command("PORT 1 CFG BAUD 9600");
    TEST_ASSERT_TRUE(contains(r, "ERR BUSY"));
}

void test_port_show_includes_seq_and_enable_flag(void) {
    FakeWriter writer;
    QuadUartController c(&writer);

    const std::string r = c.handle_command("PORT 0 SHOW");
    TEST_ASSERT_TRUE(contains(r, "PORT=0"));
    TEST_ASSERT_TRUE(contains(r, "BAUD="));
    TEST_ASSERT_TRUE(contains(r, "FORMAT="));
    TEST_ASSERT_TRUE(contains(r, "LEN="));
    TEST_ASSERT_TRUE(contains(r, "PPS="));
    TEST_ASSERT_TRUE(contains(r, "EN="));
    TEST_ASSERT_TRUE(contains(r, "SEQ="));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_format_8e2);
    RUN_TEST(test_parse_format_reject_invalid);
    RUN_TEST(test_cfg_rejected_when_any_port_enabled);
    RUN_TEST(test_port_show_includes_seq_and_enable_flag);
    return UNITY_END();
}
