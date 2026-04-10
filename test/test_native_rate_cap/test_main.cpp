#include <unity.h>

void test_placeholder(void) {
    TEST_ASSERT_TRUE(false);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_placeholder);
    return UNITY_END();
}
