/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * gimp-gegl-utils.h
 * Copyright (C) 2007 Michael Natterer <mitch@gimp.org>
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

#include <gegl.h>

#include "gimp-gegl-types.h"

#include "core/gimpprogress.h"

#include "gimp-gegl-utils.h"


const gchar *
gimp_interpolation_to_gegl_filter (GimpInterpolationType interpolation)
{
  switch (interpolation)
    {
    case GIMP_INTERPOLATION_NONE:    return "nearest";
    case GIMP_INTERPOLATION_LINEAR:  return "linear";
    case GIMP_INTERPOLATION_CUBIC:   return "cubic";
    case GIMP_INTERPOLATION_LOHALO:  return "lohalo";
    default:
      break;
    }

  return "nearest";
}

GeglColor *
gimp_gegl_color_new (const GimpRGB *rgb)
{
  GeglColor *color;

  g_return_val_if_fail (rgb != NULL, NULL);

  color = gegl_color_new (NULL);
  gegl_color_set_pixel (color, babl_format ("R'G'B'A double"), rgb);

  return color;
}

static void
gimp_gegl_progress_notify (GObject          *object,
                           const GParamSpec *pspec,
                           GimpProgress     *progress)
{
  const gchar *text;
  gdouble      value;

  g_object_get (object, "progress", &value, NULL);

  text = g_object_get_data (object, "gimp-progress-text");

  if (text)
    {
      if (value == 0.0)
        {
          gimp_progress_start (progress, text, FALSE);
          return;
        }
      else if (value == 1.0)
        {
          gimp_progress_end (progress);
          return;
        }
    }

  gimp_progress_set_value (progress, value);
}

void
gimp_gegl_progress_connect (GeglNode     *node,
                            GimpProgress *progress,
                            const gchar  *text)
{
  GObject *operation = NULL;

  g_return_if_fail (GEGL_IS_NODE (node));
  g_return_if_fail (GIMP_IS_PROGRESS (progress));

  g_object_get (node, "gegl-operation", &operation, NULL);

  g_return_if_fail (operation != NULL);

  g_signal_connect (operation, "notify::progress",
                    G_CALLBACK (gimp_gegl_progress_notify),
                    progress);

  if (text)
    g_object_set_data_full (operation,
                            "gimp-progress-text", g_strdup (text),
                            (GDestroyNotify) g_free);

  g_object_unref (operation);
}
