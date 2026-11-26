/* Included after the actual production get_reg fallback and struct regstat.
 * Tests byte-offset fields, all signed IDs, excluded register and no mutation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

static unsigned checks;
static void check_map(signed char *map)
{
    signed char saved[HOST_REGS];
    int wanted, i, expected;
    memcpy(saved, map, sizeof saved);
    for (wanted = -128; wanted <= 127; ++wanted) {
        expected = -1;
        for (i = HOST_REGS - 1; i >= 0; --i)
            if (i != EXCLUDE_REG && map[i] == wanted)
                expected = i;
        assert(get_reg(map, (signed char)wanted) == expected);
        assert(memcmp(saved, map, sizeof saved) == 0);
        ++checks;
    }
}

int main(void)
{
    signed char storage[HOST_REGS + 16];
    struct regstat state;
    int offset, index, value;
    assert(offsetof(struct regstat, regmap) == 13);
    for (offset = 0; offset < 16; ++offset) {
        signed char *map = storage + offset;
        for (index = 0; index < HOST_REGS; ++index) {
            for (value = -128; value <= 127; ++value) {
                memset(map, -1, HOST_REGS);
                map[index] = (signed char)value;
                check_map(map);
            }
        }
        for (index = 0; index < HOST_REGS; ++index)
            map[index] = (signed char)(index + 20);
        check_map(map);
    }
    memset(&state, 0, sizeof state);
    for (index = 0; index < HOST_REGS; ++index)
        state.regmap[index] = (signed char)(index + 40);
    check_map(state.regmap);
    printf("PS1 production C lookup: %u signed-ID checks, offsets 0..15, struct field +13, no mutation.\n", checks);
    return 0;
}
