#define R_NO_REMAP
#include <Rinternals.h>

#include "compat.h"


static SEXP callbacks = NULL;

// Preallocate a callback
static void push_callback(SEXP stack) {
  SEXP top = CDR(stack);

  SEXP early_handler = PROTECT(Rf_allocVector(LGLSXP, 1));
  SEXP fn_extptr = PROTECT(cleancall_MakeExternalPtrFn(NULL, R_NilValue,
                                                       R_NilValue));
  SEXP data_extptr = PROTECT(R_MakeExternalPtr(NULL, early_handler,
                                               R_NilValue));
  SEXP cb = Rf_cons(Rf_cons(fn_extptr, data_extptr), top);

  SETCDR(stack, cb);

  UNPROTECT(3);
}

struct data_wrapper {
  SEXP (*fn)(void* data);
  void *data;
  SEXP callbacks;
  int success;
};

static void call_exits(void* data) {
  // Remove protecting node. Don't remove the preallocated callback on
  // the top as it might contain a handler when something went wrong.
  SEXP top = CDR(callbacks);

  // Restore old stack
  struct data_wrapper* state = data;
  callbacks = (SEXP) state->callbacks;

  // Handlers should not jump
  while (top != R_NilValue) {
    SEXP cb = CAR(top);
    top = CDR(top);

    void (*fn)(void*) = (void (*)(void*)) R_ExternalPtrAddrFn(CAR(cb));
    void *data = (void*) EXTPTR_PTR(CDR(cb));
    int early_handler = LOGICAL(EXTPTR_TAG(CDR(cb)))[0];

    // Check for empty pointer in preallocated callbacks
    if (fn) {
      if (!early_handler || !state->success) fn(data);
    }
  }
}

static SEXP with_cleanup_context_wrap(void *data) {
  struct data_wrapper* cdata = data;
  SEXP ret = cdata->fn(cdata->data);
  cdata->success = 1;
  return ret;
}

SEXP r_with_cleanup_context(SEXP (*fn)(void* data), void* data) {
  // Preallocate new stack before changing `callbacks` to avoid
  // leaving the global variable in a bad state if alloc fails
  SEXP new = PROTECT(Rf_cons(R_NilValue, R_NilValue));
  push_callback(new);

  SEXP old = callbacks;
  callbacks = new;

  struct data_wrapper state = { fn, data, old, 0 };

  SEXP out = R_ExecWithCleanup(with_cleanup_context_wrap, &state,
                               &call_exits, &state);

  UNPROTECT(1);
  return out;
}

static void call_save_handler(void (*fn)(void *data), void* data,
                              int early) {
  if (!callbacks) {
    fn(data);
    Rf_error("Internal error: Exit handler pushed outside "
             "of an exit context");
  }

  SEXP cb = CADR(callbacks);

  // Update pointers
  cleancall_SetExternalPtrAddrFn(CAR(cb), (DL_FUNC) fn);
  R_SetExternalPtrAddr(CDR(cb), data);
  LOGICAL(EXTPTR_TAG(CDR(cb)))[0] = early;

  // Preallocate the next callback in case the allocator jumps
  push_callback(callbacks);
}

void r_call_on_exit(void (*fn)(void* data), void* data) {
  call_save_handler(fn, data, /* early = */ 0);
}

void r_call_on_early_exit(void (*fn)(void* data), void* data) {
  call_save_handler(fn, data, /* early = */ 1);
}
