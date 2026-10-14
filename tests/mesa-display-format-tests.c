/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define CHECK(expr) do { if (!(expr)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(2); \
} } while (0)
struct wsi_display { int fd; };
static unsigned held_properties, held_property, held_blob, queries, allocations;
static unsigned fail_allocation;
static bool fail_properties, fail_property, fail_blob, no_blob_property;
static uint32_t property_ids[] = {10, 11}, formats[130];
static uint64_t property_values[] = {0, 44};
static drmModeObjectProperties properties;
static drmModePropertyRes property;
static drmModePropertyBlobRes blob;
static struct {
   struct drm_format_modifier_blob header;
   uint32_t formats[130];
   struct drm_format_modifier modifiers[6];
} data;

static drmModeObjectProperties *get_properties(int fd, uint32_t id, uint32_t type)
{
   CHECK(fd == 9 && id == 7 && type == DRM_MODE_OBJECT_PLANE && !held_properties);
   if (fail_properties) return NULL;
   held_properties++;
   properties = (drmModeObjectProperties){2, property_ids, property_values};
   return &properties;
}
static drmModePropertyPtr get_property(int fd, uint32_t id)
{
   CHECK(fd == 9 && held_properties && !held_property);
   queries++;
   if (fail_property && id == 11) return NULL;
   held_property++;
   memset(&property, 0, sizeof(property));
   strcpy(property.name, id == 11 && !no_blob_property ? "IN_FORMATS" : "other");
   return &property;
}
static drmModePropertyBlobPtr get_blob(int fd, uint32_t id)
{
   CHECK(fd == 9 && id == 44 && !held_blob);
   if (fail_blob) return NULL;
   held_blob++;
   blob = (drmModePropertyBlobRes){44, sizeof(data), &data};
   return &blob;
}
static void free_properties(drmModeObjectProperties *p) { CHECK(p == &properties && held_properties == 1); held_properties--; }
static void free_property(drmModePropertyPtr p) { CHECK(p == &property && held_property == 1); held_property--; }
static void free_blob(drmModePropertyBlobPtr p) { CHECK(p == &blob && held_blob == 1); held_blob--; }
static void *resize(void *p, size_t size)
{
   if (++allocations == fail_allocation) return NULL;
   return realloc(p, size);
}
#define drmModeObjectGetProperties get_properties
#define drmModeGetProperty get_property
#define drmModeGetPropertyBlob get_blob
#define drmModeFreeObjectProperties free_properties
#define drmModeFreeProperty free_property
#define drmModeFreePropertyBlob free_blob
#define realloc resize
#define mesa_loge(...) ((void)0)
#include "display-format-under-test.h"
#undef realloc

int main(int argc, char **argv)
{
   CHECK(argc == 2);
   unsigned cases = 0, failures = 0;
   struct wsi_display wsi = {9};
   data.header.count_formats = 130;
   data.header.formats_offset = offsetof(__typeof__(data), formats);
   data.header.count_modifiers = 6;
   data.header.modifiers_offset = offsetof(__typeof__(data), modifiers);
   for (unsigned i = 0; i < 130; ++i) data.formats[i] = formats[i] = 1000 + i;
   for (unsigned i = 0; i < 6; ++i) {
      data.modifiers[i].offset = (i / 2) * 64;
      data.modifiers[i].formats = i % 2 ? UINT64_C(0x5555555555555555) : UINT64_MAX;
      data.modifiers[i].modifier = i + 900;
   }
   if (!strcmp(argv[1], "missing-property")) {
      fail_property = true;
      uint32_t count = 91;
      uint64_t *mods = wsi_get_modifiers_for_format(&wsi, 7, 1000, &count);
      CHECK(!mods && !count && !held_properties && !held_property && !held_blob);
      puts("DISPLAY_MISSING_PROPERTY_PASS");
      return 0;
   }
   CHECK(!strcmp(argv[1], "formats"));
   for (unsigned repeat = 0; repeat < 8; ++repeat) {
      for (unsigned index = 0; index < 132; ++index) {
         for (unsigned error = 0; error < 6; ++error) {
            cases++;
            fail_properties = error == 1; fail_blob = error == 2;
            no_blob_property = error == 3; fail_allocation = error >= 4 ? error - 3 : 0;
            allocations = queries = 0;
            uint32_t count = 91;
            uint32_t format = index < 130 ? formats[index] : 999;
            uint64_t expected[6]; unsigned expected_count = 0;
            if (index < 130 && error != 1 && error != 2 && error != 3) {
               for (unsigned i = 0; i < 6; ++i) {
                  unsigned bit = index - data.modifiers[i].offset;
                  if (index >= data.modifiers[i].offset && bit < 64 &&
                      (data.modifiers[i].formats & (UINT64_C(1) << bit)))
                     expected[expected_count++] = data.modifiers[i].modifier;
               }
               if (fail_allocation && fail_allocation <= expected_count) expected_count = 0;
            }
            uint64_t *mods = wsi_get_modifiers_for_format(&wsi, 7, format, &count);
            bool valid = !held_properties && !held_property && !held_blob && count == expected_count;
            if (expected_count) valid = valid && mods && !memcmp(mods, expected, expected_count * sizeof(*mods));
            else valid = valid && !mods;
            if (!valid) failures++;
            free(mods);
            held_properties = held_property = held_blob = 0;
         }
      }
   }
   printf("DISPLAY_FORMAT_CASES=%u FAILURES=%u\n", cases, failures);
   return failures ? 1 : 0;
}
