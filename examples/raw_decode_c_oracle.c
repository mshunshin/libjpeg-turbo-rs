/*
 * Reference oracle for raw component-plane decode (`Decoder::decode_raw`).
 *
 * `decode_raw` documents itself as libjpeg's `jpeg_read_raw_data()`: the
 * decoded component planes at their own (possibly subsampled) resolution,
 * with no upsampling and no colour conversion. Neither `djpeg` nor
 * TurboJPEG's YUV API exposes that call directly (TurboJPEG pads and
 * re-plans the planes), so this program drives libjpeg with
 * `raw_data_out = TRUE` and reports each plane's active area for
 * `fuzz/fuzz_targets/fuzz_raw_diff_c.rs`.
 *
 * usage: raw_decode_c_oracle <infile>     ("/dev/stdin" reads a pipe)
 *
 * Output on stdout: `RAW <components>\n`, then for each component
 * `<width> <height>\n` followed by width*height samples, row by row. The
 * width and height are libjpeg's `downsampled_width` / `downsampled_height`;
 * the MCU padding beyond them is not reported, since what a decoder leaves
 * there is not part of the image. libjpeg's own error handler reports a
 * refusal on stderr and exits non-zero; its first warning, if any, also goes
 * to stderr.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeglib.h"

int main(int argc, char **argv)
{
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  FILE *in;
  JSAMPARRAY rows[MAX_COMPONENTS];
  JSAMPLE *planes[MAX_COMPONENTS];
  JDIMENSION plane_w[MAX_COMPONENTS], plane_h[MAX_COMPONENTS];
  JDIMENSION rows_per_call[MAX_COMPONENTS];
  JDIMENSION imcu_rows, imcu_row = 0, lines_per_imcu;
  int ci;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <infile>\n", argv[0]);
    return 2;
  }
  in = fopen(argv[1], "rb");
  if (in == NULL) {
    perror("fopen");
    return 1;
  }

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, in);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.raw_data_out = TRUE;
  jpeg_start_decompress(&cinfo);

  lines_per_imcu = (JDIMENSION)(cinfo.max_v_samp_factor * cinfo.min_DCT_v_scaled_size);
  imcu_rows = (cinfo.output_height + lines_per_imcu - 1) / lines_per_imcu;
  for (ci = 0; ci < cinfo.num_components; ci++) {
    jpeg_component_info *comp = &cinfo.comp_info[ci];
    rows_per_call[ci] = (JDIMENSION)(comp->v_samp_factor * comp->DCT_v_scaled_size);
    plane_w[ci] = comp->width_in_blocks * (JDIMENSION)comp->DCT_h_scaled_size;
    plane_h[ci] = imcu_rows * rows_per_call[ci];
    rows[ci] = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                          plane_w[ci], rows_per_call[ci]);
    planes[ci] = (JSAMPLE *)calloc((size_t)plane_w[ci] * plane_h[ci], 1);
    if (planes[ci] == NULL) {
      perror("calloc");
      return 1;
    }
  }

  while (cinfo.output_scanline < cinfo.output_height) {
    JDIMENSION r;
    if (jpeg_read_raw_data(&cinfo, rows, lines_per_imcu) == 0)
      break; /* suspension cannot happen with a stdio source */
    for (ci = 0; ci < cinfo.num_components; ci++)
      for (r = 0; r < rows_per_call[ci]; r++)
        memcpy(planes[ci] + ((size_t)imcu_row * rows_per_call[ci] + r) * plane_w[ci],
               rows[ci][r], plane_w[ci]);
    imcu_row++;
  }

  printf("RAW %d\n", cinfo.num_components);
  for (ci = 0; ci < cinfo.num_components; ci++) {
    jpeg_component_info *comp = &cinfo.comp_info[ci];
    JDIMENSION y;
    printf("%u %u\n", comp->downsampled_width, comp->downsampled_height);
    for (y = 0; y < comp->downsampled_height; y++) {
      if (fwrite(planes[ci] + (size_t)y * plane_w[ci], 1, comp->downsampled_width,
                 stdout) != comp->downsampled_width) {
        perror("fwrite");
        return 1;
      }
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  fclose(in);
  /* A short write must fail the oracle, not hand the caller a truncated
   * reference that it would then compare against. */
  if (fflush(stdout) != 0) {
    perror("fflush");
    return 1;
  }
  return 0;
}
