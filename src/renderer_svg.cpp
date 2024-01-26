#include "renderer_svg.h"

#include <fmt/ostream.h>

#include <cmath>
#include <functional>

#include "base_64.h"
#include "compress.h"
#include "uuid.h"

namespace unigd
{
namespace renderers
{

static inline void write_xml_escaped(fmt::memory_buffer &os, const std::string &text)
{
  for (const char &c : text)
  {
    switch (c)
    {
      case '&':
        fmt::format_to(std::back_inserter(os), "&amp;");
        break;
      case '<':
        fmt::format_to(std::back_inserter(os), "&lt;");
        break;
      case '>':
        fmt::format_to(std::back_inserter(os), "&gt;");
        break;
      case '"':
        fmt::format_to(std::back_inserter(os), "&quot;");
        break;
      case '\'':
        fmt::format_to(std::back_inserter(os), "&apos;");
        break;
      default:
        fmt::format_to(std::back_inserter(os), "{}", c);
    }
  }
}

static inline void css_fill_or_none(fmt::memory_buffer &os, color_t col)
{
  int alpha = color::alpha(col);
  if (alpha == 0)
  {
    fmt::format_to(std::back_inserter(os), "fill: none;");
  }
  else
  {
    fmt::format_to(std::back_inserter(os), "fill: #{:02X}{:02X}{:02X};", color::red(col),
                   color::green(col), color::blue(col));
    if (alpha != 255)
    {
      fmt::format_to(std::back_inserter(os), "fill-opacity: {:.2f};", alpha / 255.0);
    }
  }
}

static inline void css_fill_or_omit(fmt::memory_buffer &os, color_t col)
{
  int alpha = color::alpha(col);
  if (alpha != 0)
  {
    fmt::format_to(std::back_inserter(os), "fill: #{:02X}{:02X}{:02X};", color::red(col),
                   color::green(col), color::blue(col));
    if (alpha != 255)
    {
      fmt::format_to(std::back_inserter(os), "fill-opacity: {:.2f};", alpha / 255.0);
    }
  }
}

static inline double scale_lty(int lty, double lwd)
{
  // Don't rescale if lwd < 1
  // https://github.com/wch/r-source/blob/master/src/library/grDevices/src/cairo/cairoFns.c#L134
  return ((lwd > 1) ? lwd : 1) * (lty & 15);
}
static inline void css_lineinfo(fmt::memory_buffer &os, const LineInfo &line)
{
  // 1 lwd = 1/96", but units in rest of document are 1/72"
  fmt::format_to(std::back_inserter(os), "stroke-width: {:.2f};", line.lwd / 96.0 * 72);

  // Default is "stroke: #000000;" as declared in <style>
  if (line.col != color::rgba(0, 0, 0, 255))
  {
    int alpha = color::alpha(line.col);
    if (alpha == 0)
    {
      fmt::format_to(std::back_inserter(os), "stroke: none;");
    }
    else
    {
      fmt::format_to(std::back_inserter(os), "stroke: #{:02X}{:02X}{:02X};",
                     color::red(line.col), color::green(line.col), color::blue(line.col));
      if (alpha != color::byte_mask)
      {
        fmt::format_to(std::back_inserter(os), "stroke-opacity: {:.2f};",
                       color::byte_frac(alpha));
      }
    }
  }

  // Set line pattern type
  int lty = line.lty;
  switch (lty)
  {
    case LineInfo::LTY::BLANK:  // never called: blank lines never get to this point
    case LineInfo::LTY::SOLID:  // default svg setting, so don't need to write out
      break;
    default:
      // For details
      // https://github.com/wch/r-source/blob/trunk/src/include/R_ext/GraphicsEngine.h#L337
      fmt::format_to(std::back_inserter(os), " stroke-dasharray: ");
      // First number
      fmt::format_to(std::back_inserter(os), "{:.2f}", scale_lty(lty, line.lwd));
      lty = lty >> 4;
      // Remaining numbers
      for (int i = 1; i < 8 && lty & 15; i++)
      {
        fmt::format_to(std::back_inserter(os), ", {:.2f}", scale_lty(lty, line.lwd));
        lty = lty >> 4;
      }
      fmt::format_to(std::back_inserter(os), ";");
      break;
  }

  // Set line end shape
  switch (line.lend)
  {
    case LineInfo::GC_ROUND_CAP:  // declared to be default in <style>
      break;
    case LineInfo::GC_BUTT_CAP:
      fmt::format_to(std::back_inserter(os), "stroke-linecap: butt;");
      break;
    case LineInfo::GC_SQUARE_CAP:
      fmt::format_to(std::back_inserter(os), "stroke-linecap: square;");
      break;
    default:
      break;
  }

  // Set line join shape
  switch (line.ljoin)
  {
    case LineInfo::GC_ROUND_JOIN:  // declared to be default in <style>
      break;
    case LineInfo::GC_BEVEL_JOIN:
      fmt::format_to(std::back_inserter(os), "stroke-linejoin: bevel;");
      break;
    case LineInfo::GC_MITRE_JOIN:
      fmt::format_to(std::back_inserter(os), "stroke-linejoin: miter;");
      if (std::fabs(line.lmitre - 10.0) > 1e-3)
      {  // 10 is declared to be the default in <style>
        fmt::format_to(std::back_inserter(os), "stroke-miterlimit: {:.2f};", line.lmitre);
      }
      break;
    default:
      break;
  }
}

RendererSVG::RendererSVG(std::experimental::optional<std::string> t_extra_css)
    : os(), m_extra_css(t_extra_css)
{
}

void RendererSVG::render(const Page &t_page, double t_scale)
{
  m_scale = t_scale;
  this->page(t_page);
}

void RendererSVG::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = reinterpret_cast<const uint8_t *>(os.begin());
  *t_size = os.size();
}

void RendererSVG::page(const Page &t_page)
{
  os.reserve((t_page.dcs.size() + t_page.cps.size()) * 128 + 512);
  fmt::format_to(
      std::back_inserter(os),
      R""(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" class="httpgd" )"");
  fmt::format_to(std::back_inserter(os),
                 R""(width="{:.2f}" height="{:.2f}" viewBox="0 0 {:.2f} {:.2f}")"",
                 t_page.size.x * m_scale, t_page.size.y * m_scale, t_page.size.x,
                 t_page.size.y);
  fmt::format_to(std::back_inserter(os),
                 ">\n<defs>\n"
                 "  <style type='text/css'><![CDATA[\n"
                 "    .httpgd line, .httpgd polyline, .httpgd polygon, .httpgd path, "
                 ".httpgd rect, .httpgd circle {{\n"
                 "      fill: none;\n"
                 "      stroke: #000000;\n"
                 "      stroke-linecap: round;\n"
                 "      stroke-linejoin: round;\n"
                 "      stroke-miterlimit: 10.00;\n"
                 "    }}\n");
  if (m_extra_css)
  {
    fmt::format_to(std::back_inserter(os), "{}\n", *m_extra_css);
  }
  fmt::format_to(std::back_inserter(os), "  ]]></style>\n");

  for (const auto &cp : t_page.cps)
  {
    fmt::format_to(
        std::back_inserter(os),
        R""(<clipPath id="c{:d}"><rect x="{:.2f}" y="{:.2f}" width="{:.2f}" height="{:.2f}"/></clipPath>)""
        "\n",
        cp.id, cp.rect.x, cp.rect.y, cp.rect.width, cp.rect.height);
  }
  fmt::format_to(
      std::back_inserter(os),
      "</defs>\n"
      R""(<rect width="100%" height="100%" style="stroke: none;)"");

  css_fill_or_none(os, t_page.fill);
  
  fmt::format_to(
      std::back_inserter(os),
      R""("/>)""
      "\n");

  clip_id_t last_id = t_page.cps.front().id;
  fmt::format_to(std::back_inserter(os),
                 R""(<g clip-path="url(#c{:d})">)""
                 "\n",
                 last_id);
  for (const auto &dc : t_page.dcs)
  {
    if (dc->clip_id != last_id)
    {
      fmt::format_to(std::back_inserter(os),
                     R""(</g><g clip-path="url(#c{:d})">)""
                     "\n",
                     dc->clip_id);
      last_id = dc->clip_id;
    }
    dc->visit(this);
    fmt::format_to(std::back_inserter(os), "\n");
  }
  fmt::format_to(std::back_inserter(os), "</g>\n</svg>");
}

void RendererSVG::visit(const Text *t_text)
{
  // If we specify the clip path inside <image>, the "transform" also
  // affects the clip path, so we need to specify clip path at an outer level
  // (according to svglite)
  fmt::format_to(std::back_inserter(os), "<g><text ");

  if (t_text->rot == 0.0)
  {
    fmt::format_to(std::back_inserter(os), R""(x="{:.2f}" y="{:.2f}" )"", t_text->pos.x,
                   t_text->pos.y);
  }
  else
  {
    fmt::format_to(std::back_inserter(os),
                   R""(transform="translate({:.2f},{:.2f}) rotate({:.2f})" )"",
                   t_text->pos.x, t_text->pos.y, t_text->rot * -1.0);
  }

  if (t_text->hadj == 0.5)
  {
    fmt::format_to(std::back_inserter(os), R""(text-anchor="middle" )"");
  }
  else if (t_text->hadj == 1)
  {
    fmt::format_to(std::back_inserter(os), R""(text-anchor="end" )"");
  }

  fmt::format_to(std::back_inserter(os), "style=\"");
  fmt::format_to(std::back_inserter(os), "font-family: {};font-size: {:.2f}px;",
                 t_text->text.font_family, t_text->text.fontsize);

  if (t_text->text.weight != 400)
  {
    if (t_text->text.weight == 700)
    {
      fmt::format_to(std::back_inserter(os), "font-weight: bold;");
    }
    else
    {
      fmt::format_to(std::back_inserter(os), "font-weight: {};", t_text->text.weight);
    }
  }
  if (t_text->text.italic)
  {
    fmt::format_to(std::back_inserter(os), "font-style: italic;");
  }
  if (t_text->col != (int)color::rgb(0, 0, 0))
  {
    css_fill_or_none(os, t_text->col);
  }
  if (t_text->text.features.length() > 0)
  {
    fmt::format_to(std::back_inserter(os), "font-feature-settings: {};",
                   t_text->text.features);
  }
  fmt::format_to(std::back_inserter(os), "\"");
  if (t_text->text.txtwidth_px > 0)
  {
    fmt::format_to(std::back_inserter(os),
                   R""( textLength="{:.2f}px" lengthAdjust="spacingAndGlyphs")"",
                   t_text->text.txtwidth_px);
  }
  fmt::format_to(std::back_inserter(os), ">");
  write_xml_escaped(os, t_text->str);
  fmt::format_to(std::back_inserter(os), "</text></g>");
}

void RendererSVG::visit(const Circle *t_circle)
{
  fmt::format_to(std::back_inserter(os), "<circle ");
  fmt::format_to(std::back_inserter(os), R""(cx="{:.2f}" cy="{:.2f}" r="{:.2f}" )"",
                 t_circle->pos.x, t_circle->pos.y, t_circle->radius);

  fmt::format_to(std::back_inserter(os), "style=\"");
  css_lineinfo(os, t_circle->line);
  css_fill_or_omit(os, t_circle->fill);
  fmt::format_to(std::back_inserter(os), "\"/>");
}

void RendererSVG::visit(const Line *t_line)
{
  fmt::format_to(std::back_inserter(os), "<line ");
  fmt::format_to(std::back_inserter(os),
                 R""(x1="{:.2f}" y1="{:.2f}" x2="{:.2f}" y2="{:.2f}" )"", t_line->orig.x,
                 t_line->orig.y, t_line->dest.x, t_line->dest.y);

  fmt::format_to(std::back_inserter(os), "style=\"");
  css_lineinfo(os, t_line->line);
  fmt::format_to(std::back_inserter(os), "\"/>");
}

void RendererSVG::visit(const Rect *t_rect)
{
  fmt::format_to(std::back_inserter(os), "<rect ");
  fmt::format_to(std::back_inserter(os),
                 R""(x="{:.2f}" y="{:.2f}" width="{:.2f}" height="{:.2f}" )"",
                 t_rect->rect.x, t_rect->rect.y, t_rect->rect.width, t_rect->rect.height);

  fmt::format_to(std::back_inserter(os), "style=\"");
  css_lineinfo(os, t_rect->line);
  css_fill_or_omit(os, t_rect->fill);
  fmt::format_to(std::back_inserter(os), "\"/>");
}

void RendererSVG::visit(const Polyline *t_polyline)
{
  fmt::format_to(std::back_inserter(os), "<polyline points=\"");
  for (auto it = t_polyline->points.begin(); it != t_polyline->points.end(); ++it)
  {
    if (it != t_polyline->points.begin())
    {
      fmt::format_to(std::back_inserter(os), " ");
    }
    fmt::format_to(std::back_inserter(os), "{:.2f},{:.2f}", it->x, it->y);
  }
  fmt::format_to(std::back_inserter(os), "\" style=\"");
  css_lineinfo(os, t_polyline->line);
  fmt::format_to(std::back_inserter(os), "\"/>");
}

void RendererSVG::visit(const Polygon *t_polygon)
{
  fmt::format_to(std::back_inserter(os), "<polygon points=\"");
  for (auto it = t_polygon->points.begin(); it != t_polygon->points.end(); ++it)
  {
    if (it != t_polygon->points.begin())
    {
      fmt::format_to(std::back_inserter(os), " ");
    }
    fmt::format_to(std::back_inserter(os), "{:.2f},{:.2f}", it->x, it->y);
  }
  fmt::format_to(std::back_inserter(os), "\" ");

  fmt::format_to(std::back_inserter(os), "style=\"");
  css_lineinfo(os, t_polygon->line);
  css_fill_or_omit(os, t_polygon->fill);
  fmt::format_to(std::back_inserter(os), "\" ");

  fmt::format_to(std::back_inserter(os), "/>");
}

void RendererSVG::visit(const Path *t_path)
{
  fmt::format_to(std::back_inserter(os), "<path d=\"");

  auto it_poly = t_path->nper.begin();
  std::size_t left = 0;
  for (auto it = t_path->points.begin(); it != t_path->points.end(); ++it)
  {
    if (left == 0)
    {
      left = (*it_poly) - 1;
      ++it_poly;
      fmt::format_to(std::back_inserter(os), "M{:.2f} {:.2f}", it->x, it->y);
    }
    else
    {
      --left;
      fmt::format_to(std::back_inserter(os), "L{:.2f} {:.2f}", it->x, it->y);

      if (left == 0)
      {
        fmt::format_to(std::back_inserter(os), "Z");
      }
    }
  }

  // Finish path data
  fmt::format_to(std::back_inserter(os), "\" style=\"");
  css_lineinfo(os, t_path->line);
  css_fill_or_omit(os, t_path->fill);
  fmt::format_to(std::back_inserter(os), "fill-rule: ");
  fmt::format_to(std::back_inserter(os), t_path->winding ? "nonzero" : "evenodd");
  fmt::format_to(std::back_inserter(os), ";\"/>");
}

void RendererSVG::visit(const Raster *t_raster)
{
  // If we specify the clip path inside <image>, the "transform" also
  // affects the clip path, so we need to specify clip path at an outer level
  // (according to svglite)
  fmt::format_to(std::back_inserter(os), "<g><image ");
  fmt::format_to(std::back_inserter(os),
                 R""( x="{:.2f}" y="{:.2f}" width="{:.2f}" height="{:.2f}" )"",
                 t_raster->rect.x, t_raster->rect.y, t_raster->rect.width,
                 t_raster->rect.height);
  fmt::format_to(std::back_inserter(os), R""(preserveAspectRatio="none" )"");
  if (!t_raster->interpolate)
  {
    fmt::format_to(std::back_inserter(os), R""(image-rendering="pixelated" )"");
  }
  if (t_raster->rot != 0)
  {
    fmt::format_to(std::back_inserter(os),
                   R""(transform="rotate({:.2f},{:.2f},{:.2f})" )"", -1.0 * t_raster->rot,
                   t_raster->rect.x, t_raster->rect.y);
  }
  fmt::format_to(std::back_inserter(os), " xlink:href=\"data:image/png;base64,");
  fmt::format_to(std::back_inserter(os), raster_base64(*t_raster));
  fmt::format_to(std::back_inserter(os), "\"/></g>");
}

// Portable SVG renderer

static inline void att_fill_or_none(fmt::memory_buffer &os, color_t col)
{
  int alpha = color::alpha(col);
  if (alpha == 0)
  {
    fmt::format_to(std::back_inserter(os), R""( fill="none")"");
  }
  else
  {
    fmt::format_to(std::back_inserter(os), R""( fill="#{:02X}{:02X}{:02X}")"",
                   color::red(col), color::green(col), color::blue(col));
    if (alpha != color::byte_mask)
    {
      fmt::format_to(std::back_inserter(os), R""( fill-opacity="{:.2f}")"",
                     color::byte_frac(alpha));
    }
  }
}

static inline void att_lineinfo(fmt::memory_buffer &os, const LineInfo &line)
{
  // 1 lwd = 1/96", but units in rest of document are 1/72"
  fmt::format_to(std::back_inserter(os), R""(stroke-width="{:.2f}")"",
                 line.lwd / 96.0 * 72);

  // Default is "stroke: none;"
  color_t alpha = color::alpha(line.col);
  if (alpha != 0)
  {
    fmt::format_to(std::back_inserter(os), R""( stroke="#{:02X}{:02X}{:02X}")"",
                   color::red(line.col), color::green(line.col), color::blue(line.col));
    if (alpha != color::byte_mask)
    {
      fmt::format_to(std::back_inserter(os), R""( stroke-opacity="{:.2f}")"",
                     color::byte_frac(alpha));
    }
  }

  // Set line pattern type
  int lty = line.lty;
  switch (lty)
  {
    case LineInfo::LTY::BLANK:  // never called: blank lines never get to this point
    case LineInfo::LTY::SOLID:  // default svg setting, so don't need to write out
      break;
    default:
      // For details
      // https://github.com/wch/r-source/blob/trunk/src/include/R_ext/GraphicsEngine.h#L337
      fmt::format_to(std::back_inserter(os), R""( stroke-dasharray="{:.2f})"",
                     scale_lty(lty, line.lwd));
      lty = lty >> 4;
      // Remaining numbers
      for (int i = 1; i < 8 && lty & 15; i++)
      {
        fmt::format_to(std::back_inserter(os), ", {:.2f}", scale_lty(lty, line.lwd));
        lty = lty >> 4;
      }
      fmt::format_to(std::back_inserter(os), "\"");
      break;
  }

  // Set line end shape
  switch (line.lend)
  {
    case LineInfo::GC_ROUND_CAP:
      fmt::format_to(std::back_inserter(os), R""( stroke-linecap="round")"");
      break;
    case LineInfo::GC_BUTT_CAP:
      // SVG default
      break;
    case LineInfo::GC_SQUARE_CAP:
      fmt::format_to(std::back_inserter(os), R""( stroke-linecap="square")"");
      break;
    default:
      break;
  }

  // Set line join shape
  switch (line.ljoin)
  {
    case LineInfo::GC_ROUND_JOIN:
      fmt::format_to(std::back_inserter(os), R""( stroke-linejoin="round")"");
      break;
    case LineInfo::GC_BEVEL_JOIN:
      fmt::format_to(std::back_inserter(os), R""( stroke-linejoin="bevel")"");
      break;
    case LineInfo::GC_MITRE_JOIN:
      // default
      if (std::fabs(line.lmitre - 4.0) > 1e-3)
      {  // 4 is the SVG default
        fmt::format_to(std::back_inserter(os), R""( stroke-miterlimit="{:.2f}")"",
                       line.lmitre);
      }
      break;
    default:
      break;
  }
}

RendererSVGPortable::RendererSVGPortable() : os() {}

void RendererSVGPortable::render(const Page &t_page, double t_scale)
{
  m_unique_id = unigd::uuid::uuid();
  m_scale = t_scale;
  this->page(t_page);
}

void RendererSVGPortable::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = reinterpret_cast<const uint8_t *>(os.begin());
  *t_size = os.size();
}

void RendererSVGPortable::page(const Page &t_page)
{
  os.reserve((t_page.dcs.size() + t_page.cps.size()) * 128 + 512);
  fmt::format_to(
      std::back_inserter(os),
      R""(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" class="httpgd" )"");
  fmt::format_to(std::back_inserter(os),
                 R""(width="{:.2f}" height="{:.2f}" viewBox="0 0 {:.2f} {:.2f}">)""
                 "\n<defs>\n",
                 t_page.size.x * m_scale, t_page.size.y * m_scale, t_page.size.x,
                 t_page.size.y);

  for (const auto &cp : t_page.cps)
  {
    fmt::format_to(
        std::back_inserter(os),
        R""(<clipPath id="c{:d}-{}"><rect x="{:.2f}" y="{:.2f}" width="{:.2f}" height="{:.2f}"/></clipPath>)""
        "\n",
        cp.id, m_unique_id, cp.rect.x, cp.rect.y, cp.rect.width, cp.rect.height);
  }
  fmt::format_to(std::back_inserter(os), "</defs>\n");
  fmt::format_to(
      std::back_inserter(os),
      R""(<rect width="100%" height="100%" stroke="none" fill="#{:02X}{:02X}{:02X}"/>)""
      "\n",
      color::red(t_page.fill), color::green(t_page.fill), color::blue(t_page.fill));

  clip_id_t last_id = t_page.cps.front().id;
  fmt::format_to(std::back_inserter(os),
                 R""(<g clip-path="url(#c{:d}-{})">)""
                 "\n",
                 last_id, m_unique_id);
  for (const auto &dc : t_page.dcs)
  {
    if (dc->clip_id != last_id)
    {
      fmt::format_to(std::back_inserter(os),
                     R""(</g><g clip-path="url(#c{:d}-{})">)""
                     "\n",
                     dc->clip_id, m_unique_id);
      last_id = dc->clip_id;
    }
    dc->visit(this);
    fmt::format_to(std::back_inserter(os), "\n");
  }
  fmt::format_to(std::back_inserter(os), "</g>\n</svg>");
}

void RendererSVGPortable::visit(const Rect *t_rect)
{
  fmt::format_to(std::back_inserter(os), "<rect ");
  fmt::format_to(std::back_inserter(os),
                 R""(x="{:.2f}" y="{:.2f}" width="{:.2f}" height="{:.2f}" )"",
                 t_rect->rect.x, t_rect->rect.y, t_rect->rect.width, t_rect->rect.height);

  att_lineinfo(os, t_rect->line);
  att_fill_or_none(os, t_rect->fill);
  fmt::format_to(std::back_inserter(os), "/>");
}

void RendererSVGPortable::visit(const Text *t_text)
{
  // If we specify the clip path inside <image>, the "transform" also
  // affects the clip path, so we need to specify clip path at an outer level
  // (according to svglite)
  fmt::format_to(std::back_inserter(os), "<g><text ");

  if (t_text->rot == 0.0)
  {
    fmt::format_to(std::back_inserter(os), R""(x="{:.2f}" y="{:.2f}" )"", t_text->pos.x,
                   t_text->pos.y);
  }
  else
  {
    fmt::format_to(std::back_inserter(os),
                   R""(transform="translate({:.2f},{:.2f}) rotate({:.2f})" )"",
                   t_text->pos.x, t_text->pos.y, t_text->rot * -1.0);
  }

  if (t_text->hadj == 0.5)
  {
    fmt::format_to(std::back_inserter(os), R""(text-anchor="middle" )"");
  }
  else if (t_text->hadj == 1)
  {
    fmt::format_to(std::back_inserter(os), R""(text-anchor="end" )"");
  }

  fmt::format_to(std::back_inserter(os), R""(font-family="{}" font-size="{:.2f}px")"",
                 t_text->text.font_family, t_text->text.fontsize);

  if (t_text->text.weight != 400)
  {
    if (t_text->text.weight == 700)
    {
      fmt::format_to(std::back_inserter(os), R""( font-weight="bold")"");
    }
    else
    {
      fmt::format_to(std::back_inserter(os), R""( font-weight="{}")"",
                     t_text->text.weight);
    }
  }
  if (t_text->text.italic)
  {
    fmt::format_to(std::back_inserter(os), R""( font-style="italic")"");
  }
  if (t_text->col != color::rgb(0, 0, 0))
  {
    att_fill_or_none(os, t_text->col);
  }
  if (t_text->text.features.length() > 0)
  {
    fmt::format_to(std::back_inserter(os), R""( font-feature-settings="{}")"",
                   t_text->text.features);
  }
  if (t_text->text.txtwidth_px > 0)
  {
    fmt::format_to(std::back_inserter(os),
                   R""( textLength="{:.2f}px" lengthAdjust="spacingAndGlyphs")"",
                   t_text->text.txtwidth_px);
  }
  fmt::format_to(std::back_inserter(os), ">");
  write_xml_escaped(os, t_text->str);
  fmt::format_to(std::back_inserter(os), "</text></g>");
}

void RendererSVGPortable::visit(const Circle *t_circle)
{
  fmt::format_to(std::back_inserter(os), "<circle ");
  fmt::format_to(std::back_inserter(os), R""(cx="{:.2f}" cy="{:.2f}" r="{:.2f}" )"",
                 t_circle->pos.x, t_circle->pos.y, t_circle->radius);

  att_lineinfo(os, t_circle->line);
  att_fill_or_none(os, t_circle->fill);
  fmt::format_to(std::back_inserter(os), "/>");
}

void RendererSVGPortable::visit(const Line *t_line)
{
  fmt::format_to(std::back_inserter(os), "<line ");
  fmt::format_to(std::back_inserter(os),
                 R""(x1="{:.2f}" y1="{:.2f}" x2="{:.2f}" y2="{:.2f}" )"", t_line->orig.x,
                 t_line->orig.y, t_line->dest.x, t_line->dest.y);

  att_lineinfo(os, t_line->line);
  fmt::format_to(std::back_inserter(os), "/>");
}

void RendererSVGPortable::visit(const Polyline *t_polyline)
{
  fmt::format_to(std::back_inserter(os), "<polyline points=\"");
  for (auto it = t_polyline->points.begin(); it != t_polyline->points.end(); ++it)
  {
    if (it != t_polyline->points.begin())
    {
      fmt::format_to(std::back_inserter(os), " ");
    }
    fmt::format_to(std::back_inserter(os), "{:.2f},{:.2f}", it->x, it->y);
  }
  fmt::format_to(std::back_inserter(os), "\" fill=\"none\" ");
  att_lineinfo(os, t_polyline->line);
  fmt::format_to(std::back_inserter(os), "/>");
}

void RendererSVGPortable::visit(const Polygon *t_polygon)
{
  fmt::format_to(std::back_inserter(os), "<polygon points=\"");
  for (auto it = t_polygon->points.begin(); it != t_polygon->points.end(); ++it)
  {
    if (it != t_polygon->points.begin())
    {
      fmt::format_to(std::back_inserter(os), " ");
    }
    fmt::format_to(std::back_inserter(os), "{:.2f},{:.2f}", it->x, it->y);
  }
  fmt::format_to(std::back_inserter(os), "\" ");
  att_lineinfo(os, t_polygon->line);
  att_fill_or_none(os, t_polygon->fill);
  fmt::format_to(std::back_inserter(os), "/>");
}

void RendererSVGPortable::visit(const Path *t_path)
{
  fmt::format_to(std::back_inserter(os), "<path d=\"");

  auto it_poly = t_path->nper.begin();
  std::size_t left = 0;
  for (auto it = t_path->points.begin(); it != t_path->points.end(); ++it)
  {
    if (left == 0)
    {
      left = (*it_poly) - 1;
      ++it_poly;
      fmt::format_to(std::back_inserter(os), "M{:.2f} {:.2f}", it->x, it->y);
    }
    else
    {
      --left;
      fmt::format_to(std::back_inserter(os), "L{:.2f} {:.2f}", it->x, it->y);

      if (left == 0)
      {
        fmt::format_to(std::back_inserter(os), "Z");
      }
    }
  }

  // Finish path data
  fmt::format_to(std::back_inserter(os), "\" ");
  att_lineinfo(os, t_path->line);
  att_fill_or_none(os, t_path->fill);
  fmt::format_to(std::back_inserter(os), " fill-rule=\"");
  fmt::format_to(std::back_inserter(os), t_path->winding ? "nonzero" : "evenodd");
  fmt::format_to(std::back_inserter(os), "\"/>");
}

void RendererSVGPortable::visit(const Raster *t_raster)
{
  // If we specify the clip path inside <image>, the "transform" also
  // affects the clip path, so we need to specify clip path at an outer level
  // (according to svglite)
  fmt::format_to(std::back_inserter(os), "<g><image ");
  fmt::format_to(std::back_inserter(os),
                 R""( x="{:.2f}" y="{:.2f}" width="{:.2f}" height="{:.2f}" )"",
                 t_raster->rect.x, t_raster->rect.y, t_raster->rect.width,
                 t_raster->rect.height);
  fmt::format_to(std::back_inserter(os), R""(preserveAspectRatio="none" )"");
  if (!t_raster->interpolate)
  {
    fmt::format_to(std::back_inserter(os), R""(image-rendering="pixelated" )"");
  }
  if (t_raster->rot != 0)
  {
    fmt::format_to(std::back_inserter(os),
                   R""(transform="rotate({:.2f},{:.2f},{:.2f})" )"", -1.0 * t_raster->rot,
                   t_raster->rect.x, t_raster->rect.y);
  }
  fmt::format_to(std::back_inserter(os), " xlink:href=\"data:image/png;base64,");
  fmt::format_to(std::back_inserter(os), raster_base64(*t_raster));
  fmt::format_to(std::back_inserter(os), "\"/></g>");
}

RendererSVGZ::RendererSVGZ(std::experimental::optional<std::string> t_extra_css)
    : RendererSVG(t_extra_css)
{
}

void RendererSVGZ::render(const Page &t_page, double t_scale)
{
  RendererSVG::render(t_page, t_scale);

  const uint8_t *buf;
  size_t buf_size;
  RendererSVG::get_data(&buf, &buf_size);

  m_compressed = compr::compress(buf, buf_size);
}

void RendererSVGZ::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = m_compressed.data();
  *t_size = m_compressed.size();
}

RendererSVGZPortable::RendererSVGZPortable() : RendererSVGPortable() {}

void RendererSVGZPortable::render(const Page &t_page, double t_scale)
{
  RendererSVGPortable::render(t_page, t_scale);

  const uint8_t *buf;
  size_t buf_size;
  RendererSVGPortable::get_data(&buf, &buf_size);

  m_compressed = compr::compress(buf, buf_size);
}

void RendererSVGZPortable::get_data(const uint8_t **t_buf, size_t *t_size) const
{
  *t_buf = m_compressed.data();
  *t_size = m_compressed.size();
}

}  // namespace renderers
}  // namespace unigd
