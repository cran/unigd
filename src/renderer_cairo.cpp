#include "renderer_cairo.h"

#ifndef UNIGD_NO_CAIRO

#include <cairo-pdf.h>
#include <cairo-ps.h>

#include <sstream>

#include "base_64.h"  // for RendererCairoPngBase64

#ifndef UNIGD_NO_TIFF
#include <tiffio.hxx>
#endif

// Implementation based on grDevices::cairo
// https://github.com/wch/r-source/blob/trunk/src/library/grDevices/src/cairo/cairoFns.c

namespace unigd
{
namespace renderers
{
constexpr double MATH_PI{3.14159265358979323846};

inline void set_color(cairo_t *cr, color_t col)
{
  color_t alpha = color::alpha(col);
  double red = color::red_frac(col);
  double green = color::green_frac(col);
  double blue = color::blue_frac(col);

  /* This optimization should not be necessary, but alpha = 1 seems
 to cause image fallback in some backends */
  if (alpha == color::byte_mask)
  {
    cairo_set_source_rgb(cr, red, green, blue);
  }
  else
  {
    cairo_set_source_rgba(cr, red, green, blue, color::byte_frac(alpha));
  }
}

inline void set_linetype(cairo_t *cr, const LineInfo &line)
{
  cairo_line_cap_t lcap = CAIRO_LINE_CAP_SQUARE;
  cairo_line_join_t ljoin = CAIRO_LINE_JOIN_ROUND;
  switch (line.lend)
  {
    case LineInfo::GC_ROUND_CAP:
      lcap = CAIRO_LINE_CAP_ROUND;
      break;
    case LineInfo::GC_BUTT_CAP:
      lcap = CAIRO_LINE_CAP_BUTT;
      break;
    case LineInfo::GC_SQUARE_CAP:
      lcap = CAIRO_LINE_CAP_SQUARE;
      break;
  }
  switch (line.ljoin)
  {
    case LineInfo::GC_ROUND_JOIN:
      ljoin = CAIRO_LINE_JOIN_ROUND;
      break;
    case LineInfo::GC_MITRE_JOIN:
      ljoin = CAIRO_LINE_JOIN_MITER;
      break;
    case LineInfo::GC_BEVEL_JOIN:
      ljoin = CAIRO_LINE_JOIN_BEVEL;
      break;
  }
  cairo_set_line_width(cr, (line.lwd > 0.01 ? line.lwd : 0.01) / 96.0 * 72);
  cairo_set_line_cap(cr, lcap);
  cairo_set_line_join(cr, ljoin);
  cairo_set_miter_limit(cr, line.lmitre);

  if (line.lty == LineInfo::TY_BLANK || line.lty == renderers::LineInfo::TY_SOLID)
  {
    cairo_set_dash(cr, nullptr, 0, 0);
  }
  else
  {
    double ls[16], lwd = (line.lwd > 1) ? line.lwd : 1;
    int l;
    /* Use unsigned int otherwise right shift of 'dt'
   may not terminate for loop */
    unsigned int dt = line.lty;
    for (l = 0; dt != 0; dt >>= 4, l++) ls[l] = (dt & 0xF) * lwd / 96.0 * 72;
    cairo_set_dash(cr, ls, l, 0);
  }
}

void RendererCairo::render_page(const Page *t_page)
{
  if (!color::transparent(t_page->fill))
  {
    cairo_new_path(cr);
    cairo_rectangle(cr, 0, 0, t_page->size.x, t_page->size.y);
    set_color(cr, t_page->fill);
    cairo_fill(cr);
  }

  const auto &first_clip = t_page->cps.front();
  cairo_new_path(cr);
  cairo_rectangle(cr, first_clip.rect.x, first_clip.rect.y, first_clip.rect.width,
                  first_clip.rect.height);
  cairo_clip(cr);
  auto last_clip_id = first_clip.id;
  for (const auto &dc : t_page->dcs)
  {
    if (dc->clip_id != last_clip_id)
    {
      const auto &next_clip =
          *std::find_if(t_page->cps.begin(), t_page->cps.end(),
                        [&](const Clip &clip) { return clip.id == dc->clip_id; });

      cairo_reset_clip(
          cr);  // todo: cairo docs discourages this (but R grDevices does it)
      cairo_new_path(cr);
      cairo_rectangle(cr, next_clip.rect.x, next_clip.rect.y, next_clip.rect.width,
                      next_clip.rect.height);
      cairo_clip(cr);

      last_clip_id = next_clip.id;
    }
    dc->visit(this);
  }
}

void RendererCairo::visit(const Rect *t_rect)
{
  cairo_new_path(cr);

  cairo_rectangle(cr, t_rect->rect.x, t_rect->rect.y, t_rect->rect.width,
                  t_rect->rect.height);

  if (!color::transparent(t_rect->fill))
  {
    // const auto aa = cairo_get_antialias(cr);
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    set_color(cr, t_rect->fill);
    cairo_fill_preserve(cr);
    // cairo_set_antialias(cr, aa);
  }
  if (!color::transparent(t_rect->line.col) && t_rect->line.lty != LineInfo::LTY::BLANK)
  {
    set_linetype(cr, t_rect->line);
    set_color(cr, t_rect->line.col);
    cairo_stroke(cr);
  }
}

void RendererCairo::visit(const Text *t_text)
{
  if (color::transparent(t_text->col))
  {
    return;
  }
  cairo_save(cr);

  cairo_select_font_face(
      cr, t_text->text.font_family.c_str(),
      t_text->text.italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
      (t_text->text.weight >= 700) ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, t_text->text.fontsize);

  cairo_move_to(cr, t_text->pos.x, t_text->pos.y);
  if (t_text->hadj != 0.0 || t_text->rot != 0.0)
  {
    cairo_text_extents_t te;
    cairo_text_extents(cr, t_text->str.c_str(), &te);
    if (t_text->rot != 0.0)
    {
      cairo_rotate(cr, -t_text->rot / 180.0 * MATH_PI);
    }
    if (t_text->hadj != 0.0)
    {
      cairo_rel_move_to(cr, -te.x_advance * t_text->hadj, 0);
    }
  }
  set_color(cr, t_text->col);
  cairo_show_text(cr, t_text->str.c_str());

  cairo_restore(cr);
}

void RendererCairo::visit(const Circle *t_circle)
{
  cairo_new_path(cr);
  cairo_arc(cr, t_circle->pos.x, t_circle->pos.y,
            (t_circle->radius > 0.5 ? t_circle->radius : 0.5), 0.0, 2 * MATH_PI);

  if (!color::transparent(t_circle->fill))
  {
    set_color(cr, t_circle->fill);
    cairo_fill_preserve(cr);
  }
  if (!color::transparent(t_circle->line.col) &&
      t_circle->line.lty != LineInfo::LTY::BLANK)
  {
    set_linetype(cr, t_circle->line);
    set_color(cr, t_circle->line.col);
    cairo_stroke(cr);
  }
}

void RendererCairo::visit(const Line *t_line)
{
  if (color::transparent(t_line->line.col))
  {
    return;
  }
  cairo_new_path(cr);

  set_color(cr, t_line->line.col);
  set_linetype(cr, t_line->line);
  cairo_move_to(cr, t_line->orig.x, t_line->orig.y);
  cairo_line_to(cr, t_line->dest.x, t_line->dest.y);
  cairo_stroke(cr);
}

void RendererCairo::visit(const Polyline *t_polyline)
{
  if (color::transparent(t_polyline->line.col))
  {
    return;
  }
  cairo_new_path(cr);

  set_color(cr, t_polyline->line.col);
  set_linetype(cr, t_polyline->line);

  for (auto it = t_polyline->points.begin(); it != t_polyline->points.end(); ++it)
  {
    if (it == t_polyline->points.begin())
    {
      cairo_move_to(cr, it->x, it->y);
    }
    else
    {
      cairo_line_to(cr, it->x, it->y);
    }
  }
  cairo_stroke(cr);
}

void RendererCairo::visit(const Polygon *t_polygon)
{
  cairo_new_path(cr);
  for (auto it = t_polygon->points.begin(); it != t_polygon->points.end(); ++it)
  {
    if (it == t_polygon->points.begin())
    {
      cairo_move_to(cr, it->x, it->y);
    }
    else
    {
      cairo_line_to(cr, it->x, it->y);
    }
  }
  cairo_close_path(cr);

  if (!color::transparent(t_polygon->fill))
  {
    set_color(cr, t_polygon->fill);
    cairo_fill_preserve(cr);
  }
  if (!color::transparent(t_polygon->line.col) &&
      t_polygon->line.lty != LineInfo::LTY::BLANK)
  {
    set_linetype(cr, t_polygon->line);
    set_color(cr, t_polygon->line.col);
    cairo_stroke(cr);
  }
}

void RendererCairo::visit(const Path *t_path)
{
  cairo_new_path(cr);

  auto it_poly = t_path->nper.begin();
  std::size_t left = 0;
  for (auto it = t_path->points.begin(); it != t_path->points.end(); ++it)
  {
    if (left == 0)
    {
      left = (*it_poly) - 1;
      ++it_poly;
      cairo_move_to(cr, it->x, it->y);
    }
    else
    {
      --left;
      cairo_line_to(cr, it->x, it->y);
      if (left == 0)
      {
        cairo_close_path(cr);
      }
    }
  }

  if (!color::transparent(t_path->fill))
  {
    set_color(cr, t_path->fill);
    cairo_fill_preserve(cr);
  }
  if (!color::transparent(t_path->line.col) && t_path->line.lty != LineInfo::LTY::BLANK)
  {
    set_linetype(cr, t_path->line);
    set_color(cr, t_path->line.col);
    cairo_stroke(cr);
  }
}

void RendererCairo::visit(const Raster *t_raster)
{
  cairo_save(cr);
  cairo_translate(cr, t_raster->rect.x, t_raster->rect.y);
  cairo_rotate(cr, -t_raster->rot * MATH_PI / 180);
  cairo_scale(cr, t_raster->rect.width / t_raster->wh.x,
              t_raster->rect.height / t_raster->wh.y);

  std::vector<unsigned char> imageData(t_raster->raster.size() * 4);

  // The R ABGR needs to be converted to a Cairo ARGB
  // AND values need to by premultiplied by alpha
  for (size_t i = 0; i < t_raster->raster.size(); ++i)
  {
    const color_t alpha = color::alpha(t_raster->raster[i]);
    imageData[i * 4 + 3] = alpha;
    if (alpha < color::byte_mask)
    {
      imageData[i * 4 + 2] = color::red(t_raster->raster[i]) * alpha / color::byte_mask;
      imageData[i * 4 + 1] = color::green(t_raster->raster[i]) * alpha / color::byte_mask;
      imageData[i * 4 + 0] = color::blue(t_raster->raster[i]) * alpha / color::byte_mask;
    }
    else
    {
      imageData[i * 4 + 2] = color::red(t_raster->raster[i]);
      imageData[i * 4 + 1] = color::green(t_raster->raster[i]);
      imageData[i * 4 + 0] = color::blue(t_raster->raster[i]);
    }
  }
  cairo_surface_t *image = cairo_image_surface_create_for_data(
      imageData.data(), CAIRO_FORMAT_ARGB32, t_raster->wh.x, t_raster->wh.y,
      4 * t_raster->wh.x);

  cairo_set_source_surface(cr, image, 0, 0);
  if (t_raster->interpolate)
  {
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  }
  else
  {
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  }

  cairo_new_path(cr);
  cairo_rectangle(cr, 0, 0, t_raster->wh.x, t_raster->wh.y);
  cairo_clip(cr);
  cairo_paint(cr);

  cairo_restore(cr);
  cairo_surface_destroy(image);
}

// TARGETS

static cairo_status_t cairowrite_fmt(void *closure, unsigned char const *data,
                                     unsigned int length)
{
  auto *os = static_cast<fmt::memory_buffer *>(closure);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  std::string s(reinterpret_cast<char const *>(data), length);
  fmt::format_to(std::back_inserter(*os), "{}", s);
  return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t cairowrite_ucvec(void *closure, unsigned char const *data,
                                       unsigned int length)
{
  auto *render_data = static_cast<std::vector<unsigned char> *>(closure);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  render_data->insert(render_data->end(), data, data + length);
  return CAIRO_STATUS_SUCCESS;
}

void RendererCairoPng::render(const Page &t_page, double t_scale)
{
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, t_page.size.x * t_scale,
                                       t_page.size.y * t_scale);

  cr = cairo_create(surface);

  cairo_scale(cr, t_scale, t_scale);

  render_page(&t_page);

  cairo_surface_write_to_png_stream(surface, cairowrite_ucvec, &m_render_data);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

void RendererCairoPng::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = &m_render_data[0];
  *t_size = m_render_data.size();
}

void RendererCairoPngBase64::render(const Page &t_page, double t_scale)
{
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, t_page.size.x * t_scale,
                                       t_page.size.y * t_scale);

  cr = cairo_create(surface);

  cairo_scale(cr, t_scale, t_scale);

  render_page(&t_page);

  std::vector<unsigned char> png_buf;
  cairo_surface_write_to_png_stream(surface, cairowrite_ucvec, &png_buf);
  m_buf = base64_encode(png_buf.data(), png_buf.size());
  m_buf.insert(0, "data:image/png;base64,");  // potentially very expensive

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

void RendererCairoPngBase64::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = reinterpret_cast<const unsigned char *>(m_buf.data());
  *t_size = m_buf.size();
}

void RendererCairoPdf::render(const Page &t_page, double t_scale)
{
  surface = cairo_pdf_surface_create_for_stream(
      cairowrite_ucvec, &m_render_data, t_page.size.x * t_scale, t_page.size.y * t_scale);

  cr = cairo_create(surface);

  cairo_scale(cr, t_scale, t_scale);

  render_page(&t_page);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

void RendererCairoPdf::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = &m_render_data[0];
  *t_size = m_render_data.size();
}

void RendererCairoPs::render(const Page &t_page, double t_scale)
{
  surface = cairo_ps_surface_create_for_stream(
      cairowrite_fmt, &m_os, t_page.size.x * t_scale, t_page.size.y * t_scale);

  cr = cairo_create(surface);

  cairo_scale(cr, t_scale, t_scale);

  render_page(&t_page);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

void RendererCairoPs::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = reinterpret_cast<const uint8_t *>(m_os.begin());
  *t_size = m_os.size();
}

void RendererCairoEps::render(const Page &t_page, double t_scale)
{
  surface = cairo_ps_surface_create_for_stream(
      cairowrite_fmt, &m_os, t_page.size.x * t_scale, t_page.size.y * t_scale);
  cairo_ps_surface_set_eps(surface, true);

  cr = cairo_create(surface);

  cairo_scale(cr, t_scale, t_scale);

  render_page(&t_page);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

void RendererCairoEps::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = reinterpret_cast<const uint8_t *>(m_os.begin());
  *t_size = m_os.size();
}

#ifndef UNIGD_NO_TIFF

// see: https://research.cs.wisc.edu/graphics/Courses/638-f1999/libtiff_tutorial.htm
void RendererCairoTiff::render(const Page &t_page, double t_scale)
{
  const int argb_size = 4;
  const int width = t_page.size.x * t_scale;
  const int height = t_page.size.y * t_scale;
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

  std::vector<unsigned char> raw_buffer(stride * height);
  surface = cairo_image_surface_create_for_data(raw_buffer.data(), CAIRO_FORMAT_ARGB32,
                                                width, height, stride);

  cr = cairo_create(surface);
  cairo_scale(cr, t_scale, t_scale);
  render_page(&t_page);

  std::ostringstream tiff_ostream;
  TIFF *tiff = TIFFStreamOpen("memory", &tiff_ostream);  // filename is ignored

  TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, argb_size);
  TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);

  // Used to be COMPRESSION_DEFLATE but:
  // TIFFWriteDirectorySec: Warning, Creating TIFF with legacy Deflate codec identifier,
  // COMPRESSION_ADOBE_DEFLATE is more widely supported.
  TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
  const uint16_t extras[] = {EXTRASAMPLE_ASSOCALPHA};
  TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, EXTRASAMPLE_ASSOCALPHA, extras);
  TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff, width * argb_size));

  std::vector<unsigned char> line(width * argb_size);
  for (int row = 0; row < height; ++row)
  {
    for (int x = 0; x < width * argb_size; x += argb_size)
    {
      const int raw_offset = stride * row + x;
      line[x] = raw_buffer[raw_offset + 2];
      line[x + 1] = raw_buffer[raw_offset + 1];
      line[x + 2] = raw_buffer[raw_offset];
      line[x + 3] = raw_buffer[raw_offset + 3];
    }
    if (TIFFWriteScanline(tiff, line.data(), row) < 0)
    {
      break;
    }
  }

  TIFFClose(tiff);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  const auto out = tiff_ostream.str();
  m_render_data.assign(out.begin(), out.end());
}

void RendererCairoTiff::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = &m_render_data[0];
  *t_size = m_render_data.size();
}

#endif

}  // namespace renderers
}  // namespace unigd

#endif
