#ifndef __UNIGD_RENDERERS_H__
#define __UNIGD_RENDERERS_H__

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "draw_data.h"
#include "unigd_external.h"

namespace unigd
{
namespace renderers
{
// regex for refactoring:
// ([Rr]ect|[Tt]ext|[Cc]ircle|[Ll]ine|[Pp]olyline|[Pp]olygon|[Pp]ath|[Rr]aster)

class render_target : public ex::render_data
{
 public:
  virtual void render(const Page &t_page, double t_scale) = 0;
};

using renderer_gen = std::function<std::unique_ptr<render_target>()>;
struct renderer_map_entry
{
  unigd_renderer_info info;
  renderer_gen generator;
};

bool find(const std::string &id, renderer_map_entry *renderer);
bool find_generator(const std::string &id, renderer_gen *renderer);
bool find_info(const std::string &id, unigd_renderer_info *renderer);
const std::unordered_map<std::string, renderer_map_entry> *renderers();
}  // namespace renderers

}  // namespace unigd

#endif /* __UNIGD_RENDERERS_H__ */
