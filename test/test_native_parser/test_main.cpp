#include <unity.h>

#include <cstring>
#include <string>

#include "command_parser.h"
#include "quad_uart_controller.h"

class FakeWriter : public UartTxWriter {
public:
    bool begin_port(uint8_t, const PortConfig&) override { return true; }
    int available_for_write(uint8_t) override { return 0; }
    size_t write_byte(uint8_t, uint8_t) override { return 0; }
};

bool contains(const char* haystack, const char* needle) {
    return std::strstr(haystack, needle) != nullptr;
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

    const char* r = c.handle_command("PORT 0 ENABLE", 0);
    TEST_ASSERT_EQUAL_STRING("OK", r);

    r = c.handle_command("PORT 1 CFG BAUD 9600");
    TEST_ASSERT_TRUE(contains(r, "ERR BUSY"));
}

void test_port_show_includes_seq_and_enable_flag(void) {
    FakeWriter writer;
    QuadUartController c(&writer);

    const char* r = c.handle_command("PORT 0 SHOW");
    TEST_ASSERT_TRUE(contains(r, "PORT 0"));
    TEST_ASSERT_TRUE(contains(r, "BAUD:"));
    TEST_ASSERT_TRUE(contains(r, "FORMAT:"));
    TEST_ASSERT_TRUE(contains(r, "LEN:"));
    TEST_ASSERT_TRUE(contains(r, "PPS:"));
    TEST_ASSERT_TRUE(contains(r, "EN:"));
    TEST_ASSERT_TRUE(contains(r, "SEQ:"));
}

void test_cfg_rejects_overflow_numeric_value(void) {
    FakeWriter writer;
    QuadUartController c(&writer);
    const char* r = c.handle_command("PORT 0 CFG BAUD 999999999999999999999");
    TEST_ASSERT_EQUAL_STRING("ERR BAD_VALUE baud", r);
}

void test_rejects_overlong_command_line(void) {
    FakeWriter writer;
    QuadUartController c(&writer);
    std::string long_cmd = "PORT 0 SHOW";
    while (long_cmd.size() < 300) {
        long_cmd += " X";
    }
    const char* r = c.handle_command(long_cmd.c_str());
    TEST_ASSERT_EQUAL_STRING("ERR BAD_CMD line-too-long", r);
}

void test_multi_port_cfg_updates_each_target(void) {
    FakeWriter writer;
    QuadUartController c(&writer);

    const char* r = c.handle_command("PORT 1,2,3 CFG BAUD 921600");
    TEST_ASSERT_EQUAL_STRING("OK", r);

    r = c.handle_command("PORT 1 SHOW");
    TEST_ASSERT_TRUE(contains(r, "BAUD: 921600"));
    r = c.handle_command("PORT 2 SHOW");
    TEST_ASSERT_TRUE(contains(r, "BAUD: 921600"));
    r = c.handle_command("PORT 3 SHOW");
    TEST_ASSERT_TRUE(contains(r, "BAUD: 921600"));
}

void test_multi_port_cfg_is_atomic_on_validation_error(void) {
    FakeWriter writer;
    QuadUartController c(&writer);

    const char* r = c.handle_command("PORT 1 CFG FORMAT 8E2");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 1 CFG BAUD 9600");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 1 CFG PPS 1");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 1 CFG LEN 128");
    TEST_ASSERT_EQUAL_STRING("OK", r);

    r = c.handle_command("PORT 1,2 CFG PPS 7");
    TEST_ASSERT_EQUAL_STRING("ERR BAD_VALUE cfg", r);

    r = c.handle_command("PORT 1 SHOW");
    TEST_ASSERT_TRUE(contains(r, "PPS: 1"));
    r = c.handle_command("PORT 2 SHOW");
    TEST_ASSERT_TRUE(contains(r, "PPS: 10"));
}

void test_multi_port_bad_selector_rejected(void) {
    FakeWriter writer;
    QuadUartController c(&writer);

    const char* r = c.handle_command("PORT 1,,2 SHOW");
    TEST_ASSERT_EQUAL_STRING("ERR BAD_PORT value", r);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_format_8e2);
    RUN_TEST(test_parse_format_reject_invalid);
    RUN_TEST(test_cfg_rejected_when_any_port_enabled);
    RUN_TEST(test_port_show_includes_seq_and_enable_flag);
    RUN_TEST(test_cfg_rejects_overflow_numeric_value);
    RUN_TEST(test_rejects_overlong_command_line);
    RUN_TEST(test_multi_port_cfg_updates_each_target);
    RUN_TEST(test_multi_port_cfg_is_atomic_on_validation_error);
    RUN_TEST(test_multi_port_bad_selector_rejected);
    return UNITY_END();
}
