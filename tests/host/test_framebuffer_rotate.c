#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl_port/framebuffer_rotate.h"

static void expect_pixels_(const char *name,
                           const uint16_t *actual,
                           const uint16_t *expected,
                           size_t count)
{
    if (memcmp(actual, expected, count * sizeof(*actual)) != 0) {
        fprintf(stderr, "%s failed\n", name);
        exit(1);
    }
}

int main(void)
{
    const uint16_t src[] = {
        0x0001, 0x0002, 0x0003,
        0x0004, 0x0005, 0x0006,
    };
    uint16_t normal[6] = {0};
    uint16_t rotated[6] = {0};
    uint16_t partial[6] = {0};
    const uint16_t expected_rotated[] = {
        0x0006, 0x0005, 0x0004,
        0x0003, 0x0002, 0x0001,
    };
    const uint16_t expected_partial[] = {
        0x0000, 0x0005, 0x0004,
        0x0000, 0x0002, 0x0001,
    };

    framebuffer_copy_region_rgb565(src, normal, 3U, 2U,
                                   0U, 0U, 2U, 1U, false);
    expect_pixels_("normal copy", normal, src, 6U);

    framebuffer_copy_region_rgb565(src, rotated, 3U, 2U,
                                   0U, 0U, 2U, 1U, true);
    expect_pixels_("full rotation", rotated, expected_rotated, 6U);

    framebuffer_copy_region_rgb565(src, partial, 3U, 2U,
                                   0U, 0U, 1U, 1U, true);
    expect_pixels_("partial rotation", partial, expected_partial, 6U);

    framebuffer_copy_region_rgb565(NULL, normal, 3U, 2U,
                                   0U, 0U, 2U, 1U, true);
    framebuffer_copy_region_rgb565(src, normal, 3U, 2U,
                                   2U, 0U, 1U, 1U, true);

    puts("Framebuffer region transform tests passed");
    return 0;
}
