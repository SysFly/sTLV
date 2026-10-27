#include <stdio.h>

#include "stlv.h"
#include "tinytest.h"

int tt_checks;
int tt_failures;

void suite_codec(void);
void suite_rx(void);
void suite_fault(void);

int main(void)
{
    printf("sTLV tests: TYPE_SIZE=%d LEN_SIZE=%d MAX_PAYLOAD=%d CRC_TABLE=%d\n", STLV_TYPE_SIZE,
           STLV_LEN_SIZE, STLV_MAX_PAYLOAD, STLV_CRC_TABLE);
    suite_codec();
    suite_rx();
    suite_fault();
    printf("%d checks, %d failures\n", tt_checks, tt_failures);
    return tt_failures != 0;
}
