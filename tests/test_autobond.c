/* Host-only regression test for bond selection: cc -std=c11 -Wall -Wextra -Werror tests/test_autobond.c -o test_autobond && ./test_autobond */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t bd_addr_t[6];
static const bd_addr_t bonds[2]={{2,18,52,86,120,154},{68,51,34,17,0,153}};
static bd_addr_t selected;
static bool has_phone;
static unsigned bond_count;
static void restore(unsigned count) {
    has_phone=false; bond_count=0; memset(selected,0,6);
    for (unsigned i=0;i<count;i++) {
        ++bond_count;
        if (!has_phone) { memcpy(selected,bonds[i],6); has_phone=true; }
    }
}
int main(void) {
    restore(0); assert(!has_phone && bond_count==0);
    restore(1); assert(has_phone && bond_count==1 && !memcmp(selected,bonds[0],6));
    restore(2); assert(has_phone && bond_count==2 && !memcmp(selected,bonds[0],6));
    puts("PASS: zero, one, and multiple bond-selection cases");
    return 0;
}
