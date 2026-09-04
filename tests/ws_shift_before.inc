/* ws_shift_before.inc -- the FROZEN reference implementation of ws_shift().
 *
 * Checked in verbatim and never edited, because a regression test whose
 * reference implementation can drift is not a regression test. Only the name
 * differs from the original. The test that uses it is
 * tests/widescreen_shift_test.c.
 */
static void ws_shift_before(void *dst, const void *src, unsigned count)
{
    unsigned i;

    memcpy(dst, src, (size_t)count * WS_VSIZE);
    for (i = 0; i < count; i++) {
        float *x = (float *)((char *)dst + (size_t)i * WS_VSIZE);

        /* Inside the ortho path, the game's 2D field is [0, N] and anything
         * outside it is the one-pixel overhang the cabinet's own clip dropped.
         * Clamp rather than clip: a clamp is a pure function of x, so two
         * triangles sharing a vertex get the same answer and no mesh cracks. */
        if (s_ws_ortho_depth) {
            if (*x < 0.0f)                          *x = 0.0f;
            else if (*x > (float)s_ws_native_width) *x = (float)s_ws_native_width;
        }

        float y = *(const float *)((const char *)dst + (size_t)i * WS_VSIZE + 4);

        if (*x < s_ws_bx0) s_ws_bx0 = *x;
        if (*x > s_ws_bx1) s_ws_bx1 = *x;
        if (y < s_ws_by0) s_ws_by0 = y;
        if (y > s_ws_by1) s_ws_by1 = y;
        if (*x < s_ws_seen_min) s_ws_seen_min = *x;
        if (*x > s_ws_seen_max) s_ws_seen_max = *x;
        if (*x < 0.0f)                            s_ws_left_verts++;
        else if (*x > (float)s_ws_native_width)   s_ws_right_verts++;
        *x += s_ws_dx;
    }
    s_ws_seen_verts += count;
    if (s_ws_seen_verts >= 200000u && !s_ws_extent_reported) {
        s_ws_extent_reported = 1;
        LOGI("geometry: widescreen extent -- %u vertices: %.2f%% left of 0, "
             "%.2f%% right of %d, range %.1f .. %.1f. A widened frustum fills "
             "%.1f .. %.1f and should put several per cent in each margin; a "
             "trickle means the culling is still native and only overhang is "
             "escaping.",
             s_ws_seen_verts,
             100.0 * (double)s_ws_left_verts / (double)s_ws_seen_verts,
             100.0 * (double)s_ws_right_verts / (double)s_ws_seen_verts,
             s_ws_native_width, (double)s_ws_seen_min, (double)s_ws_seen_max,
             /* what pass 1 widened the frustum TO -- the raster centred on the
              * field, which is half a pixel wider each side than the whole-pixel
              * origin the vertices are then translated by */
             (double)(s_ws_native_width - s_ws_cull_width) * 0.5,
             (double)(s_ws_native_width + s_ws_cull_width) * 0.5);
    }
}
