#include <unity.h>

#include <array>
#include <vector>

#include "quad_uart_controller.h"

class FakeWriter : public UartTxWriter {
public:
    bool begin_port(uint8_t port_id, const PortConfig& config) override {
        (void)config;
        if (port_id >= kPortCount) {
            return false;
        }
        began_[port_id] = true;
        return true;
    }

    int available_for_write(uint8_t port_id) override {
        if (port_id >= kPortCount) {
            return 0;
        }
        return credits_[port_id];
    }

    size_t write_byte(uint8_t port_id, uint8_t byte) override {
        if (port_id >= kPortCount) {
            return 0;
        }
        if (credits_[port_id] == 0) {
            return 0;
        }
        writes_[port_id].push_back(byte);
        credits_[port_id]--;
        return 1;
    }

    void set_credits(uint8_t port_id, int credits) {
        credits_[port_id] = credits;
    }

private:
    std::array<int, kPortCount> credits_{};
    std::array<bool, kPortCount> began_{};
    std::array<std::vector<uint8_t>, kPortCount> writes_{};
};

void test_compute_max_pps_9600_8e2_128(void) {
    const uint16_t max_pps = compute_max_pps(9600, 8, ParityMode::Even, 2, 128);
    TEST_ASSERT_EQUAL_UINT16(6, max_pps);
}

void test_cfg_pps_rejected_above_dynamic_cap(void) {
    FakeWriter writer;
    QuadUartController c(&writer);
    const char* r = nullptr;

    r = c.handle_command("PORT 0 CFG PPS 1");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 CFG BAUD 9600");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 CFG FORMAT 8E2");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 CFG LEN 128");
    TEST_ASSERT_EQUAL_STRING("OK", r);

    r = c.handle_command("PORT 0 CFG PPS 7");
    TEST_ASSERT_EQUAL_STRING("ERR BAD_VALUE cfg", r);

    r = c.handle_command("PORT 0 CFG PPS 6");
    TEST_ASSERT_EQUAL_STRING("OK", r);
}

void test_cfg_rejects_pps_above_hard_cap(void) {
    FakeWriter writer;
    QuadUartController c(&writer);
    const char* r = nullptr;

    r = c.handle_command("PORT 0 CFG BAUD 921600");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 CFG FORMAT 8N1");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 CFG LEN 10");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 CFG PPS 1001");
    TEST_ASSERT_EQUAL_STRING("ERR BAD_VALUE cfg", r);
}

void test_seq_advances_only_after_packet_complete(void) {
    FakeWriter writer;
    QuadUartController c(&writer);
    const char* r = nullptr;

    r = c.handle_command("PORT 0 CFG LEN 10");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 CFG PPS 1");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 ENABLE", 1000000);
    TEST_ASSERT_EQUAL_STRING("OK", r);

    writer.set_credits(0, 4);
    c.service_tx_nonblocking(1000000);
    TEST_ASSERT_EQUAL_UINT32(0, c.runtime(0).seq);
    TEST_ASSERT_EQUAL_UINT8(1, c.runtime(0).tx_in_progress ? 1 : 0);

    writer.set_credits(0, 16);
    c.service_tx_nonblocking(1000001);
    TEST_ASSERT_EQUAL_UINT32(1, c.runtime(0).seq);
    TEST_ASSERT_EQUAL_UINT8(0, c.runtime(0).tx_in_progress ? 1 : 0);
}

void test_scheduler_handles_micros_wraparound(void) {
    FakeWriter writer;
    QuadUartController c(&writer);
    const char* r = nullptr;

    r = c.handle_command("PORT 0 CFG LEN 10");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 CFG PPS 1");
    TEST_ASSERT_EQUAL_STRING("OK", r);
    r = c.handle_command("PORT 0 ENABLE", 0xFFFFFF00U);
    TEST_ASSERT_EQUAL_STRING("OK", r);

    writer.set_credits(0, 10);
    c.service_tx_nonblocking(0xFFFFFF00U);
    TEST_ASSERT_EQUAL_UINT32(1, c.runtime(0).seq);

    writer.set_credits(0, 20);
    c.service_tx_nonblocking(1000U);
    TEST_ASSERT_EQUAL_UINT32(1, c.runtime(0).seq);

    writer.set_credits(0, 20);
    c.service_tx_nonblocking(1000000U);
    TEST_ASSERT_EQUAL_UINT32(2, c.runtime(0).seq);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_compute_max_pps_9600_8e2_128);
    RUN_TEST(test_cfg_pps_rejected_above_dynamic_cap);
    RUN_TEST(test_cfg_rejects_pps_above_hard_cap);
    RUN_TEST(test_seq_advances_only_after_packet_complete);
    RUN_TEST(test_scheduler_handles_micros_wraparound);
    return UNITY_END();
}
