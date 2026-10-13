/* SPDX-License-Identifier: MIT */
#include <xrt/xrt_compositor.h>
#include <stdio.h>

struct comp_layer { struct xrt_layer_data data; };
struct comp_compositor {
    struct {
        struct { unsigned layer_count; struct comp_layer layers[2]; } layer_accum;
    } base;
};
#include "projection-under-test.h"

int main(void)
{
    unsigned cases = 0, failures = 0;
    const enum xrt_layer_type types[] = {XRT_LAYER_PROJECTION, XRT_LAYER_PROJECTION_DEPTH, XRT_LAYER_QUAD};
    for (unsigned t = 0; t < 3; t++) {
        for (unsigned count = 0; count <= 2; count++) {
            for (unsigned view = 0; view < 2; view++) {
                for (unsigned color = 0; color < 2; color++) {
                    for (unsigned chroma = 0; chroma < 2; chroma++) {
                        struct comp_compositor c = {0};
                        c.base.layer_accum.layer_count = count;
                        struct xrt_layer_data *data = &c.base.layer_accum.layers[0].data;
                        data->type = types[t];
                        data->flags = (view ? XRT_LAYER_COMPOSITION_VIEW_SPACE_BIT : 0) |
                                      (color ? XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE : 0);
                        if (types[t] == XRT_LAYER_PROJECTION) data->proj.chroma_key.curve = chroma;
                        if (types[t] == XRT_LAYER_PROJECTION_DEPTH) data->depth.chroma_key.curve = chroma;
                        bool expected = t < 2 && count == 1 && !view && !color && !chroma;
                        bool actual = can_do_one_projection_layer_fast_path(&c);
                        if (actual != expected) {
                            fprintf(stderr, "type=%u count=%u view=%u color=%u chroma=%u: expected %d, got %d\n",
                                    t, count, view, color, chroma, expected, actual);
                            failures++;
                        }
                        cases++;
                    }
                }
            }
        }
    }
    printf("MONADO_PROJECTION_CASES=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
