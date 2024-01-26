#ifndef __UNIGD_R_THREAD_H__
#define __UNIGD_R_THREAD_H__

#include <future>
#include <thread>

#include "async_utils.h"

namespace unigd
{
namespace async
{
void ipc_open();
void ipc_close();

void r_thread_impl(function_wrapper &&f);

template <typename FunctionType>
std::future<typename std::result_of<FunctionType()>::type> r_thread(FunctionType f)
{
  typedef typename std::result_of<FunctionType()>::type result_type;
  std::packaged_task<result_type()> task(std::move(f));
  std::future<result_type> res(task.get_future());
  r_thread_impl(std::move(task));
  return res;
}

}  // namespace async
}  // namespace unigd

#endif /* __UNIGD_R_THREAD_H__ */
