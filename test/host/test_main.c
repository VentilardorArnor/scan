#include "test_support.h"

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    run_nmea_tests();
    run_contract_tests();
    run_journal_tests();
    run_telemetry_tx_tests();
    run_control_tests();
    run_prov_line_tests();
    run_lossy_link_tests();
    return UNITY_END();
}
