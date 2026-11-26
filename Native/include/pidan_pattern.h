#ifndef PIDAN_PATTERN_H
#define PIDAN_PATTERN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Stable C ABI. No input pointers are retained. Status: 0 success, 1 invalid input,
 * 2 not recognized, 3 invalid cell data, 4 insufficient output, 5 internal error, 6 cancelled. */
typedef struct {
  char share_id_bits[45];
  uint8_t watermark; /* 1 requests rendering the author name; occupies former ABI padding. */
  int32_t mirror;
  int32_t method;
} pidan_watermark;
/** A token belongs to one operation. cancel is thread-safe and idempotent.
 * Destroy only after the executing call has returned; cancellation does not free memory.
 * _ex returns 6 on cancellation and never publishes a partial watermark. */
typedef struct pidan_cancel_token pidan_cancel_token;
pidan_cancel_token *pidan_cancel_create(void);
void pidan_cancel_request(pidan_cancel_token *token);
int pidan_cancel_requested(const pidan_cancel_token *token);
void pidan_cancel_destroy(pidan_cancel_token *token);
int pidan_recognize_ex(const uint8_t *pixels, size_t length, int width, int height,
                       int channels, int prepared, pidan_watermark *out, pidan_cancel_token *token);
/** Recommendations only; actual capacity depends on the host and decoder. */
#define PIDAN_RECOMMENDED_INPUT_PIXELS 16000000
#define PIDAN_RECOMMENDED_INPUT_SIDE 4096
/** Largest margin-probe side; the probe fits in a 256x256 square. */
#define PIDAN_PROBE_MAX_SIDE 256
/** Probe dimensions for an original width x height: scale=min(1,256/width,256/height),
 * round half up, minimum 1, aspect ratio preserved. size receives 2 ints: probe w/h.
 * Returns 0, or 1 for invalid arguments. Adapters call this instead of duplicating it. */
int pidan_probe_size(int width, int height, int *size);
/** Probe is straight sRGB RGBA at pidan_probe_size dimensions, unrotated. plan has 6 ints:
 * crop x/y/w/h, output w/h. Rules are shared by every image adapter. No pointers are retained. */
int pidan_image_plan(const uint8_t *probe, size_t length, int width, int height, int *plan);
/** Already cropped/scaled work image, each side <=2000. Skips original-image preprocessing. */
int pidan_recognize_work_rgba(const uint8_t *pixels, size_t length, int width, int height,
                             pidan_watermark *out);
/** Packed opaque RGB. Reads caller memory directly; never expands the original to RGBA. */
int pidan_recognize_rgb(const uint8_t *pixels, size_t length, int width, int height, pidan_watermark *out);
int pidan_recognize_rgba(const uint8_t *pixels, size_t length, int width, int height,
                         pidan_watermark *out);
/** Query response grid. hex holds rows*cols cells, row-major (index r*cols+c), each exactly
 * two uppercase hex characters, concatenated without separators (length 2*rows*cols).
 * "00" is an empty cell; "01".."DD" are palette indices 1..221 (see pidan_color_code).
 * rows/cols 1..104. Writes rows*cols indices only after the whole grid validates. */
int pidan_decode_cells(const char *hex, size_t length, int rows, int cols, uint16_t *out,
                       size_t capacity);
/** Index 1..221 → frozen MARD code; 0 (empty) → ""; out of range → NULL. */
const char *pidan_color_code(unsigned index);
const char *pidan_core_version(void);
/** Shared allocator for FFI/WASM. A null allocation must be handled by the caller. */
void *pidan_alloc(size_t length);
void pidan_free(void *pointer);
#ifdef __cplusplus
}
#endif
#endif
