/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * gimpbrush-transform.c
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <glib-object.h>

#include "core-types.h"

#include "libgimpmath/gimpmath.h"

#include "gimpbrush.h"
#include "gimpbrush-transform.h"

#include "base/temp-buf.h"


/*  local function prototypes  */

static void  gimp_brush_transform_matrix       (GimpBrush         *brush,
                                                gdouble            scale_x,
                                                gdouble            scale_y,
                                                gdouble            angle,
                                                GimpMatrix3       *matrix);
static void  gimp_brush_transform_bounding_box (GimpBrush         *brush,
                                                const GimpMatrix3 *matrix,
                                                gint              *x,
                                                gint              *y,
                                                gint              *width,
                                                gint              *height);


/*  public functions  */

void
gimp_brush_real_transform_size (GimpBrush *brush,
                                gdouble    scale_x,
                                gdouble    scale_y,
                                gdouble    angle,
                                gint      *width,
                                gint      *height)
{
  GimpMatrix3 matrix;
  gint        x, y;

  gimp_brush_transform_matrix (brush, scale_x, scale_y, angle, &matrix);
  gimp_brush_transform_bounding_box (brush, &matrix, &x, &y, width, height);
}

/*
 * Transforms the brush mask with bilinear interpolation.
 *
 * Rather than calculating the inverse transform for each point in the
 * transformed image, this algorithm uses the inverse transformed corner
 * points of the destination image to work out the starting position in the
 * source image and the U and V deltas in the source image space.
 * It then uses a scan-line approach, looping through rows and colummns
 * in the transformed (destination) image while walking along the corresponding
 * rows and columns (named U and V) in the source image.
 *
 * The horizontal in destination space (transform result) is reverse transformed
 * into source image space to get U.
 * The vertical in destination space (transform result) is reverse transformed
 * into source image space to get V.
 *
 * The strength of this particular algorithm is that calculation work should
 * depend more upon the final transformed brush size rather than the input brush size.
 *
 * There are no floating point calculations in the inner loop for speed.
 */
TempBuf *
gimp_brush_real_transform_mask (GimpBrush *brush,
                                gdouble    scale_x,
                                gdouble    scale_y,
                                gdouble    angle)
{
  TempBuf      *result;
  guchar       *dest;
  const guchar *src;
  GimpMatrix3   matrix;
  gint          src_width;
  gint          src_height;
  gint          dest_width;
  gint          dest_height;
  gint          x, y;
  gdouble       blx, brx, tlx, trx;
  gdouble       bly, bry, tly, try;
  gdouble       src_tl_to_tr_delta_x;
  gdouble       src_tl_to_tr_delta_y;
  gdouble       src_tl_to_bl_delta_x;
  gdouble       src_tl_to_bl_delta_y;
  gint          src_walk_ux;
  gint          src_walk_uy;
  gint          src_walk_vx;
  gint          src_walk_vy;
  gint          src_space_cur_pos_x;
  gint          src_space_cur_pos_y;
  gint          src_space_row_start_x;
  gint          src_space_row_start_y;
  guchar       *src_walker;
  guchar       *pixel_next;
  guchar       *pixel_below;
  guchar       *pixel_below_next;
  gint          opposite_x, distance_from_true_x;
  gint          opposite_y, distance_from_true_y;
  gint          src_height_times_int_multiple;
  gint          src_width_times_int_multiple;

  /*
   * tl, tr etc are used because it is easier to visualize top left, top right etc
   * corners of the forward transformed source image rectangle.
   */
  const gint fraction_bits = 12;
  const gint int_multiple  = pow(2,fraction_bits);

  /* In inner loop's bilinear calculation, two numbers that were each previously multiplied by
   * int_multiple are multiplied together.
   * To get back the right result, the multiplication result must be
   * divided *twice* by 2^fraction_bits, equivalent to
   * bit shift right by 2 * fraction_bits
   */
  const gint recovery_bits = 2 * fraction_bits;

  /*
   * example: suppose fraction_bits = 9
   * a 9-bit mask looks like this: 0001 1111 1111
   * and is given by:  2^fraction_bits - 1
   * demonstration:
   * 2^0     = 0000 0000 0001
   * 2^1     = 0000 0000 0010
   * :
   * 2^8     = 0001 0000 0000
   * 2^9     = 0010 0000 0000
   * 2^9 - 1 = 0001 1111 1111
   */
  const guint fraction_bitmask = pow(2, fraction_bits) - 1 ;


  gimp_brush_transform_matrix (brush, scale_x, scale_y, angle, &matrix);

  if (gimp_matrix3_is_identity (&matrix))
    return temp_buf_copy (brush->mask, NULL);

  src_width  = brush->mask->width;
  src_height = brush->mask->height;

  gimp_brush_transform_bounding_box (brush, &matrix,
                                     &x, &y, &dest_width, &dest_height);
  gimp_matrix3_translate (&matrix, -x, -y);
  gimp_matrix3_invert (&matrix);

  result = temp_buf_new (dest_width, dest_height, 1, 0, 0, NULL);

  dest = temp_buf_get_data (result);
  src  = temp_buf_get_data (brush->mask);

  /* prevent disappearance of 1x1 pixel brush at some rotations when scaling < 1 */
  /*
  if (src_width == 1 && src_height == 1 && scale_x < 1 && scale_y < 1 )
    {
      *dest = src[0];
      return result;
    }*/

  gimp_matrix3_transform_point (&matrix, 0,          0,           &tlx, &tly);
  gimp_matrix3_transform_point (&matrix, dest_width, 0,           &trx, &try);
  gimp_matrix3_transform_point (&matrix, 0,          dest_height, &blx, &bly);
  gimp_matrix3_transform_point (&matrix, dest_width, dest_height, &brx, &bry);


  /* in image space, calc U (what was horizontal originally)
   * note: double precision
   */
  src_tl_to_tr_delta_x = trx - tlx;
  src_tl_to_tr_delta_y = try - tly;

  /* in image space, calc V (what was vertical originally)
   * note: double precision
   */
  src_tl_to_bl_delta_x = blx - tlx;
  src_tl_to_bl_delta_y = bly - tly;

  /* speed optimized, note conversion to int precision */
  src_walk_ux = (gint) ((src_tl_to_tr_delta_x / dest_width)* int_multiple);
  src_walk_uy = (gint) ((src_tl_to_tr_delta_y / dest_width)* int_multiple);
  src_walk_vx = (gint) ((src_tl_to_bl_delta_x / dest_height)* int_multiple);
  src_walk_vy = (gint) ((src_tl_to_bl_delta_y / dest_height)* int_multiple);

  /* initialize current position in source space to the start position (tl)
   * speed optimized, note conversion to int precision
   */
  src_space_cur_pos_x    = (gint) (tlx* int_multiple);
  src_space_cur_pos_y    = (gint) (tly* int_multiple);
  src_space_row_start_x  = (gint) (tlx* int_multiple);
  src_space_row_start_y  = (gint) (tly* int_multiple);

  src_walker = src;

  src_height_times_int_multiple = src_height << fraction_bits; /* mult by int_multiple */
  src_width_times_int_multiple  = src_width  << fraction_bits; /* mult by int_multiple */
  const gint src_heightm1_times_int_multiple = src_height_times_int_multiple - int_multiple;
  const gint src_widthm1_times_int_multiple  = src_width_times_int_multiple - int_multiple;

  for (y = 0; y < dest_height; y++)
    {
      for (x = 0; x < dest_width; x++)
        {
          if (src_space_cur_pos_x > src_width_times_int_multiple  ||
              src_space_cur_pos_x < 0     ||
              src_space_cur_pos_y > src_height_times_int_multiple ||
              src_space_cur_pos_y < 0)
              /* no corresponding pixel in source space */
            {
              *dest = 0;
            }
          else /* reverse transformed point hits source pixel */
            {
              src_walker = src
                        + (src_space_cur_pos_y>>fraction_bits) * src_width
                        + (src_space_cur_pos_x>>fraction_bits);

              /* bottom right corner
               * no pixel below, reuse current pixel instead
               * no next pixel to the right so reuse current pixel instead
               */
              if (src_space_cur_pos_y > (src_heightm1_times_int_multiple) &&
                  src_space_cur_pos_x > (src_widthm1_times_int_multiple) )
                {
                  pixel_next  = src_walker;
                  pixel_below = src_walker;
                  pixel_below_next = src_walker;
                }

              /* bottom edge pixel row, except rightmost corner
               * no pixel below, reuse current pixel instead  */
              else if (src_space_cur_pos_y > (src_heightm1_times_int_multiple))
                {
                  pixel_next  = src_walker + 1;
                  pixel_below = src_walker;
                  pixel_below_next = src_walker + 1;
                }

              /* right edge pixel column, except bottom corner
               * no next pixel to the right so reuse current pixel instead */
              else if (src_space_cur_pos_x > (src_widthm1_times_int_multiple))
                {
                  pixel_next  = src_walker;
                  pixel_below = src_walker + src_width;
                  pixel_below_next = pixel_below;
                }

              /* neither on bottom edge nor on right edge */
              else
                {
                  pixel_next  = src_walker + 1;
                  pixel_below = src_walker + src_width;
                  pixel_below_next = pixel_below + 1;
                }

              distance_from_true_x = src_space_cur_pos_x & fraction_bitmask;
              distance_from_true_y = src_space_cur_pos_y & fraction_bitmask;
              opposite_x =  int_multiple - distance_from_true_x;
              opposite_y =  int_multiple - distance_from_true_y;

              *dest= ( (src_walker[0] * opposite_x + pixel_next[0] * distance_from_true_x) * opposite_y +
                       (pixel_below[0] * opposite_x + pixel_below_next[0] *distance_from_true_x) * distance_from_true_y
                     ) >> recovery_bits;

            }

          src_space_cur_pos_x+=src_walk_ux;
          src_space_cur_pos_y+=src_walk_uy;
          dest ++;
        } /* end for x */
        src_space_row_start_x +=src_walk_vx;
        src_space_row_start_y +=src_walk_vy;
        src_space_cur_pos_x = src_space_row_start_x;
        src_space_cur_pos_y = src_space_row_start_y;

    } /* end for y */

  return result;
}


/*
 * Transforms the brush pixemap with bilinear interpolation.
 *
 * The algorithm used is exactly the same as for the brush mask
 * (gimp_brush_real_transform_mask) except it accounts for 3 color channels
 *  instead of 1 greyscale channel.
 *
 * Rather than calculating the inverse transform for each point in the
 * transformed image, this algorithm uses the inverse transformed corner
 * points of the destination image to work out the starting position in the
 * source image and the U and V deltas in the source image space.
 * It then uses a scan-line approach, looping through rows and colummns
 * in the transformed (destination) image while walking along the corresponding
 * rows and columns (named U and V) in the source image.
 *
 * The horizontal in destination space (transform result) is reverse transformed
 * into source image space to get U.
 * The vertical in destination space (transform result) is reverse transformed
 * into source image space to get V.
 *
 * The strength of this particular algorithm is that calculation work should
 * depend more upon the final transformed brush size rather than the input brush size.
 *
 * There are no floating point calculations in the inner loop for speed.
 */
TempBuf *
gimp_brush_real_transform_pixmap (GimpBrush *brush,
                                           gdouble    scale_x,
                                           gdouble    scale_y,
                                           gdouble    angle)
{
  TempBuf      *result;
  guchar       *dest;
  const guchar *src;
  GimpMatrix3   matrix;
  gint          src_width;
  gint          src_height;
  gint          dest_width;
  gint          dest_height;
  gint          x, y;
  gdouble       blx, brx, tlx, trx;
  gdouble       bly, bry, tly, try;
  gdouble       src_tl_to_tr_delta_x;
  gdouble       src_tl_to_tr_delta_y;
  gdouble       src_tl_to_bl_delta_x;
  gdouble       src_tl_to_bl_delta_y;
  gint          src_walk_ux;
  gint          src_walk_uy;
  gint          src_walk_vx;
  gint          src_walk_vy;
  gint          src_space_cur_pos_x;
  gint          src_space_cur_pos_y;
  gint          src_space_row_start_x;
  gint          src_space_row_start_y;
  guchar       *src_walker;
  guchar       *pixel_next;
  guchar       *pixel_below;
  guchar       *pixel_below_next;
  gint          opposite_x, distance_from_true_x;
  gint          opposite_y, distance_from_true_y;
  gint          src_height_times_int_multiple;
  gint          src_width_times_int_multiple;

  /*
   * tl, tr etc are used because it is easier to visualize top left, top right etc
   * corners of the forward transformed source image rectangle.
   */
  const gint fraction_bits = 12;
  const gint int_multiple  = pow(2,fraction_bits);

  /* In inner loop's bilinear calculation, two numbers that were each previously multiplied by
   * int_multiple are multiplied together.
   * To get back the right result, the multiplication result must be
   * divided *twice* by 2^fraction_bits, equivalent to
   * bit shift right by 2 * fraction_bits
   */
  const gint recovery_bits = 2 * fraction_bits;

  /*
   * example: suppose fraction_bits = 9
   * a 9-bit mask looks like this: 0001 1111 1111
   * and is given by:  2^fraction_bits - 1
   * demonstration:
   * 2^0     = 0000 0000 0001
   * 2^1     = 0000 0000 0010
   * :
   * 2^8     = 0001 0000 0000
   * 2^9     = 0010 0000 0000
   * 2^9 - 1 = 0001 1111 1111
   */
  const guint fraction_bitmask = pow(2, fraction_bits)- 1 ;


  gimp_brush_transform_matrix (brush, scale_x, scale_y, angle, &matrix);

  if (gimp_matrix3_is_identity (&matrix))
    return temp_buf_copy (brush->pixmap, NULL);

  src_width  = brush->pixmap->width;
  src_height = brush->pixmap->height;

  gimp_brush_transform_bounding_box (brush, &matrix,
                                     &x, &y, &dest_width, &dest_height);
  gimp_matrix3_translate (&matrix, -x, -y);
  gimp_matrix3_invert (&matrix);

  result = temp_buf_new (dest_width, dest_height, 3, 0, 0, NULL);

  dest = temp_buf_get_data (result);
  src  = temp_buf_get_data (brush->pixmap);

  gimp_matrix3_transform_point (&matrix, 0,          0,           &tlx, &tly);
  gimp_matrix3_transform_point (&matrix, dest_width, 0,           &trx, &try);
  gimp_matrix3_transform_point (&matrix, 0,          dest_height, &blx, &bly);
  gimp_matrix3_transform_point (&matrix, dest_width, dest_height, &brx, &bry);


  /* in image space, calc U (what was horizontal originally)
   * note: double precision
   */
  src_tl_to_tr_delta_x = trx - tlx;
  src_tl_to_tr_delta_y = try - tly;

  /* in image space, calc V (what was vertical originally)
   * note: double precision
   */
  src_tl_to_bl_delta_x = blx - tlx;
  src_tl_to_bl_delta_y = bly - tly;

  /* speed optimized, note conversion to int precision */
  src_walk_ux = (gint) ((src_tl_to_tr_delta_x / dest_width)* int_multiple);
  src_walk_uy = (gint) ((src_tl_to_tr_delta_y / dest_width)* int_multiple);
  src_walk_vx = (gint) ((src_tl_to_bl_delta_x / dest_height)* int_multiple);
  src_walk_vy = (gint) ((src_tl_to_bl_delta_y / dest_height)* int_multiple);

  /* initialize current position in source space to the start position (tl)
   * speed optimized, note conversion to int precision
   */
  src_space_cur_pos_x    = (gint) (tlx* int_multiple);
  src_space_cur_pos_y    = (gint) (tly* int_multiple);
  src_space_row_start_x  = (gint) (tlx* int_multiple);
  src_space_row_start_y  = (gint) (tly* int_multiple);

  src_walker = src;

  src_height_times_int_multiple = src_height << fraction_bits; /* mult by int_multiple */
  src_width_times_int_multiple  = src_width  << fraction_bits; /* mult by int_multiple */
  const gint src_heightm1_times_int_multiple = src_height_times_int_multiple - int_multiple;
  const gint src_widthm1_times_int_multiple  = src_width_times_int_multiple - int_multiple;

  for (y = 0; y < dest_height; y++)
    {
      for (x = 0; x < dest_width; x++)
        {
          if (src_space_cur_pos_x > src_width_times_int_multiple  ||
              src_space_cur_pos_x < 0     ||
              src_space_cur_pos_y > src_height_times_int_multiple ||
              src_space_cur_pos_y < 0)
              /* no corresponding pixel in source space */
            {
              dest[0] = 0;
              dest[1] = 0;
              dest[2] = 0;
            }
          else /* reverse transformed point hits source pixel */
            {
              src_walker = src
                        + 3 * (
                          (src_space_cur_pos_y >> fraction_bits) * src_width
                        + (src_space_cur_pos_x >> fraction_bits));

              /* bottom right corner
               * no pixel below, reuse current pixel instead
               * no next pixel to the right so reuse current pixel instead
               */
              if (src_space_cur_pos_y > (src_heightm1_times_int_multiple) &&
                  src_space_cur_pos_x > (src_widthm1_times_int_multiple) )
                {
                  pixel_next  = src_walker;
                  pixel_below = src_walker;
                  pixel_below_next = src_walker;
                }

              /* bottom edge pixel row, except rightmost corner
               * no pixel below, reuse current pixel instead  */
              else if (src_space_cur_pos_y > (src_heightm1_times_int_multiple))
                {
                  pixel_next  = src_walker + 3;
                  pixel_below = src_walker;
                  pixel_below_next = src_walker + 3;
                }

              /* right edge pixel column, except bottom corner
               * no next pixel to the right so reuse current pixel instead */
              else if (src_space_cur_pos_x > (src_widthm1_times_int_multiple))
                {
                  pixel_next  = src_walker;
                  pixel_below = src_walker + src_width * 3;
                  pixel_below_next = pixel_below;
                }

              /* neither on bottom edge nor on right edge */
              else
                {
                  pixel_next  = src_walker + 3;
                  pixel_below = src_walker + src_width * 3;
                  pixel_below_next = pixel_below + 3;
                }

              distance_from_true_x = src_space_cur_pos_x & fraction_bitmask;
              distance_from_true_y = src_space_cur_pos_y & fraction_bitmask;
              opposite_x =  int_multiple - distance_from_true_x;
              opposite_y =  int_multiple - distance_from_true_y;

              dest[0] = ((src_walker[0] * opposite_x + pixel_next[0] * distance_from_true_x) * opposite_y +
                         (pixel_below[0] * opposite_x + pixel_below_next[0] *distance_from_true_x) * distance_from_true_y
                        ) >> recovery_bits;

              dest[1] = ((src_walker[1] * opposite_x + pixel_next[1] * distance_from_true_x) * opposite_y +
                         (pixel_below[1] * opposite_x + pixel_below_next[1] *distance_from_true_x) * distance_from_true_y
                        ) >> recovery_bits;

              dest[2] = ((src_walker[2] * opposite_x + pixel_next[2] * distance_from_true_x) * opposite_y +
                         (pixel_below[2] * opposite_x + pixel_below_next[2] *distance_from_true_x) * distance_from_true_y
                        ) >> recovery_bits;
            }

          src_space_cur_pos_x += src_walk_ux;
          src_space_cur_pos_y += src_walk_uy;
          dest += 3;
        } /* end for x */
        src_space_row_start_x +=src_walk_vx;
        src_space_row_start_y +=src_walk_vy;
        src_space_cur_pos_x = src_space_row_start_x;
        src_space_cur_pos_y = src_space_row_start_y;

    } /* end for y */


  return result;
}


/*  private functions  */

static void
gimp_brush_transform_matrix (GimpBrush   *brush,
                             gdouble      scale_x,
                             gdouble      scale_y,
                             gdouble      angle,
                             GimpMatrix3 *matrix)
{
  const gdouble center_x = brush->mask->width  / 2;
  const gdouble center_y = brush->mask->height / 2;

  gimp_matrix3_identity (matrix);
  gimp_matrix3_translate (matrix, - center_x, - center_y);
  gimp_matrix3_rotate (matrix, -2 * G_PI * angle);
  gimp_matrix3_translate (matrix, center_x, center_y);
  gimp_matrix3_scale (matrix, scale_x, scale_y);
}

static void
gimp_brush_transform_bounding_box (GimpBrush         *brush,
                                   const GimpMatrix3 *matrix,
                                   gint              *x,
                                   gint              *y,
                                   gint              *width,
                                   gint              *height)
{
  const gdouble  w = brush->mask->width;
  const gdouble  h = brush->mask->height;
  gdouble        x1, x2, x3, x4;
  gdouble        y1, y2, y3, y4;

  gimp_matrix3_transform_point (matrix, 0, 0, &x1, &y1);
  gimp_matrix3_transform_point (matrix, w, 0, &x2, &y2);
  gimp_matrix3_transform_point (matrix, 0, h, &x3, &y3);
  gimp_matrix3_transform_point (matrix, w, h, &x4, &y4);

  *x = floor (MIN (MIN (x1, x2), MIN (x3, x4)));
  *y = floor (MIN (MIN (y1, y2), MIN (y3, y4)));

  *width  = (gint) (ceil  (MAX (MAX (x1, x2), MAX (x3, x4))) - *x);
  *height = (gint) (ceil  (MAX (MAX (y1, y2), MAX (y3, y4))) - *y);
}