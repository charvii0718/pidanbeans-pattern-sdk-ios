#include "internal.hpp"
#include <atomic>
#include <new>
struct pidan_cancel_token { std::atomic<bool> requested{false}; };
#ifdef __EMSCRIPTEN__
extern "C" __attribute__((import_module("env"), import_name("pidan_pause"))) void pidan_pause();
static pidan_cancel_token *active = nullptr;
#else
static thread_local pidan_cancel_token *active = nullptr;
#endif
namespace pidan {
CancelScope::CancelScope(pidan_cancel_token *token) : previous(active) { active = token; }
CancelScope::~CancelScope() { active = previous; }
bool cancelled() {
#ifdef __EMSCRIPTEN__
  pidan_pause();
#endif
  return active && active->requested.load(std::memory_order_relaxed);
}
}
extern "C" {
pidan_cancel_token *pidan_cancel_create() { return new (std::nothrow) pidan_cancel_token; }
void pidan_cancel_request(pidan_cancel_token *token) {
  if (token) token->requested.store(true, std::memory_order_relaxed);
}
int pidan_cancel_requested(const pidan_cancel_token *token) {
  return token && token->requested.load(std::memory_order_relaxed);
}
void pidan_cancel_destroy(pidan_cancel_token *token) { delete token; }
int pidan_recognize_ex(const uint8_t *p, size_t n, int w, int h, int channels,
                       int prepared, pidan_watermark *out, pidan_cancel_token *token) {
  if (out) *out = {};
  if ((channels != 3 && channels != 4) || (prepared && channels != 4)) return 1;
  pidan::CancelScope scope(token);
  if (pidan::cancelled()) return 6;
  int status = (prepared ? pidan_recognize_work_rgba : channels == 3 ? pidan_recognize_rgb : pidan_recognize_rgba)(p,n,w,h,out);
  if (pidan::cancelled()) { if (out) *out = {}; return 6; }
  return status;
}
}
