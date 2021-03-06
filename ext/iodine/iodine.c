#include "iodine.h"
#include "iodine_helpers.h"
#include "iodine_http.h"
#include "iodine_protocol.h"
#include "iodine_pubsub.h"
#include "iodine_websockets.h"
#include "rb-rack-io.h"
#include <dlfcn.h>
/*
Copyright: Boaz segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/

VALUE Iodine;
VALUE IodineBase;

VALUE iodine_force_var_id;
VALUE iodine_channel_var_id;
VALUE iodine_pattern_var_id;
VALUE iodine_text_var_id;
VALUE iodine_binary_var_id;
VALUE iodine_engine_var_id;
VALUE iodine_message_var_id;

ID iodine_fd_var_id;
ID iodine_cdata_var_id;
ID iodine_timeout_var_id;
ID iodine_call_proc_id;
ID iodine_new_func_id;
ID iodine_on_open_func_id;
ID iodine_on_message_func_id;
ID iodine_on_data_func_id;
ID iodine_on_drained_func_id;
ID iodine_on_shutdown_func_id;
ID iodine_on_close_func_id;
ID iodine_ping_func_id;
ID iodine_buff_var_id;
ID iodine_to_s_method_id;
ID iodine_to_i_func_id;

rb_encoding *IodineBinaryEncoding;
rb_encoding *IodineUTF8Encoding;
int IodineBinaryEncodingIndex;
int IodineUTF8EncodingIndex;

/* *****************************************************************************
Internal helpers
***************************************************************************** */

static void iodine_run_task(void *block_) {
  RubyCaller.call((VALUE)block_, iodine_call_proc_id);
}

static void iodine_perform_deferred(void *block_, void *ignr) {
  RubyCaller.call((VALUE)block_, iodine_call_proc_id);
  Registry.remove((VALUE)block_);
  (void)ignr;
}

/* *****************************************************************************
Published functions
***************************************************************************** */

/** Returns the number of total connections managed by Iodine. */
static VALUE iodine_count(VALUE self) {
  size_t count = facil_count(NULL);
  return ULL2NUM(count);
  (void)self;
}

/**
Runs the required block after the specified number of milliseconds have passed.
Time is counted only once Iodine started running (using {Iodine.start}).

Tasks scheduled before calling {Iodine.start} will run once for every process.

Always returns a copy of the block object.
*/
static VALUE iodine_run_after(VALUE self, VALUE milliseconds) {
  (void)(self);
  if (TYPE(milliseconds) != T_FIXNUM) {
    rb_raise(rb_eTypeError, "milliseconds must be a number");
    return Qnil;
  }
  size_t milli = FIX2UINT(milliseconds);
  // requires a block to be passed
  rb_need_block();
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qfalse;
  Registry.add(block);
  if (facil_run_every(milli, 1, iodine_run_task, (void *)block,
                      (void (*)(void *))Registry.remove) == -1) {
    perror("ERROR: Iodine couldn't initialize timer");
    return Qnil;
  }
  return block;
}
/**
Runs the required block after the specified number of milliseconds have passed.
Time is counted only once Iodine started running (using {Iodine.start}).

Accepts:

milliseconds:: the number of milliseconds between event repetitions.

repetitions:: the number of event repetitions. Defaults to 0 (never ending).

block:: (required) a block is required, as otherwise there is nothing to
perform.

The event will repeat itself until the number of repetitions had been delpeted.

Always returns a copy of the block object.
*/
static VALUE iodine_run_every(int argc, VALUE *argv, VALUE self) {
  (void)(self);
  VALUE milliseconds, repetitions, block;

  rb_scan_args(argc, argv, "11&", &milliseconds, &repetitions, &block);

  if (TYPE(milliseconds) != T_FIXNUM) {
    rb_raise(rb_eTypeError, "milliseconds must be a number.");
    return Qnil;
  }
  if (repetitions != Qnil && TYPE(repetitions) != T_FIXNUM) {
    rb_raise(rb_eTypeError, "repetitions must be a number or `nil`.");
    return Qnil;
  }

  size_t milli = FIX2UINT(milliseconds);
  size_t repeat = (repetitions == Qnil) ? 0 : FIX2UINT(repetitions);
  // requires a block to be passed
  rb_need_block();
  Registry.add(block);
  if (facil_run_every(milli, repeat, iodine_run_task, (void *)block,
                      (void (*)(void *))Registry.remove) == -1) {
    perror("ERROR: Iodine couldn't initialize timer");
    return Qnil;
  }
  return block;
}

static VALUE iodine_run(VALUE self) {
  rb_need_block();
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qfalse;
  Registry.add(block);
  defer(iodine_perform_deferred, (void *)block, NULL);
  return block;
  (void)self;
}

/* *****************************************************************************
Idling
***************************************************************************** */
#include "fio_llist.h"
#include "spnlock.inc"

static spn_lock_i iodine_on_idle_lock = SPN_LOCK_INIT;
static fio_ls_s iodine_on_idle_list = FIO_LS_INIT(iodine_on_idle_list);

/**
Schedules a single occuring event for the next idle cycle.

To schedule a reoccuring event, simply reschedule the event at the end of it's
run.

i.e.

      IDLE_PROC = Proc.new { puts "idle"; Iodine.on_idle &IDLE_PROC }
      Iodine.on_idle &IDLE_PROC
*/
VALUE iodine_sched_on_idle(VALUE self) {
  rb_need_block();
  VALUE block = rb_block_proc();
  Registry.add(block);
  spn_lock(&iodine_on_idle_lock);
  fio_ls_push(&iodine_on_idle_list, (void *)block);
  spn_unlock(&iodine_on_idle_lock);
  return block;
  (void)self;
}

static void iodine_on_idle(void) {
  spn_lock(&iodine_on_idle_lock);
  while (fio_ls_any(&iodine_on_idle_list)) {
    VALUE block = (VALUE)fio_ls_shift(&iodine_on_idle_list);
    defer(iodine_perform_deferred, (void *)block, NULL);
  }
  spn_unlock(&iodine_on_idle_lock);
}
/* *****************************************************************************
Running the server
***************************************************************************** */

#include "spnlock.inc"
#include <pthread.h>

static volatile int sock_io_thread = 0;
static pthread_t sock_io_pthread;
typedef struct {
  size_t threads;
  size_t processes;
} iodine_start_settings_s;

static void *iodine_io_thread(void *arg) {
  (void)arg;
  struct timespec tm;
  while (sock_io_thread) {
    sock_flush_all();
    tm = (struct timespec){.tv_nsec = 0, .tv_sec = 1};
    nanosleep(&tm, NULL);
  }
  return NULL;
}
static void iodine_start_io_thread(void *a1, void *a2) {
  (void)a1;
  (void)a2;
  pthread_create(&sock_io_pthread, NULL, iodine_io_thread, NULL);
}
static void iodine_join_io_thread(void) {
  sock_io_thread = 0;
  pthread_join(sock_io_pthread, NULL);
}

static void *srv_start_no_gvl(void *s_) {
  iodine_start_settings_s *s = s_;
  sock_io_thread = 1;
  defer(iodine_start_io_thread, NULL, NULL);
  fprintf(stderr, "\n");
  if (s->processes == 1 || (s->processes == 0 && s->threads > 0)) {
    /* single worker */
    RubyCaller.call(Iodine, rb_intern("before_fork"));
    RubyCaller.call(Iodine, rb_intern("after_fork"));
  }
  facil_run(.threads = s->threads, .processes = s->processes,
            .on_idle = iodine_on_idle, .on_finish = iodine_join_io_thread);
  return NULL;
}

static int iodine_review_rack_app(void) {
  /* Check for Iodine::Rack.app and perform the C equivalent:
   *  Iodine::HTTP.listen app: @app, port: @port, address: @address, log: @log,
   *          max_msg: max_msg, max_body: max_body, public: @public, ping:
   *          @ws_timeout, timeout: @timeout
   */

  VALUE rack = rb_const_get(Iodine, rb_intern("Rack"));
  VALUE app = rb_ivar_get(rack, rb_intern("@app"));
  VALUE www = rb_ivar_get(rack, rb_intern("@public"));
  if ((app == Qnil || app == Qfalse) && (www == Qnil || www == Qfalse))
    return 0;
  VALUE opt = rb_hash_new();
  Registry.add(opt);

  rb_hash_aset(opt, ID2SYM(rb_intern("app")),
               rb_ivar_get(rack, rb_intern("@app")));
  rb_hash_aset(opt, ID2SYM(rb_intern("port")),
               rb_ivar_get(rack, rb_intern("@port")));
  rb_hash_aset(opt, ID2SYM(rb_intern("app")),
               rb_ivar_get(rack, rb_intern("@app")));
  rb_hash_aset(opt, ID2SYM(rb_intern("address")),
               rb_ivar_get(rack, rb_intern("@address")));
  rb_hash_aset(opt, ID2SYM(rb_intern("log")),
               rb_ivar_get(rack, rb_intern("@log")));
  rb_hash_aset(opt, ID2SYM(rb_intern("max_msg")),
               rb_ivar_get(rack, rb_intern("@max_msg")));
  rb_hash_aset(opt, ID2SYM(rb_intern("max_body")),
               rb_ivar_get(rack, rb_intern("@max_body")));
  rb_hash_aset(opt, ID2SYM(rb_intern("public")),
               rb_ivar_get(rack, rb_intern("@public")));
  rb_hash_aset(opt, ID2SYM(rb_intern("ping")),
               rb_ivar_get(rack, rb_intern("@ws_timeout")));
  rb_hash_aset(opt, ID2SYM(rb_intern("timeout")),
               rb_ivar_get(rack, rb_intern("@ws_timeout")));
  rb_hash_aset(opt, ID2SYM(rb_intern("max_headers")),
               rb_ivar_get(rack, rb_intern("@max_headers")));
  if (rb_funcall2(Iodine, rb_intern("listen2http"), 1, &opt) == Qfalse) {
    Registry.remove(opt);
    return -1;
  }
  Registry.remove(opt);
  return 0;
}

/**
Starts the Iodine event loop. This will hang the thread until an interrupt
(`^C`) signal is received.

Returns the Iodine module.
*/
static VALUE iodine_start(VALUE self) {
  /* re-register the Rack::Push namespace to point at Iodine */
  if (rb_const_defined(rb_cObject, rb_intern("Rack"))) {
    VALUE rack = rb_const_get(rb_cObject, rb_intern("Rack"));
    if (rack != Qnil) {
      if (rb_const_defined(rack, rb_intern("PubSub"))) {
        rb_const_remove(rack, rb_intern("PubSub"));
      }
      rb_const_set(rack, rb_intern("PubSub"), Iodine);
    }
  }
  /* for the special Iodine::Rack object and backwards compatibility */
  if (iodine_review_rack_app()) {
    fprintf(stderr, "ERROR: (iodine) cann't start Iodine::Rack.\n");
    return Qnil;
  }

  VALUE rb_th_i = rb_iv_get(Iodine, "@threads");
  VALUE rb_pr_i = rb_iv_get(Iodine, "@processes");

  iodine_start_settings_s s = {
      .threads = ((TYPE(rb_th_i) == T_FIXNUM) ? FIX2INT(rb_th_i) : 0),
      .processes = ((TYPE(rb_pr_i) == T_FIXNUM) ? FIX2INT(rb_pr_i) : 0)};

  RubyCaller.set_gvl_state(1);
  RubyCaller.leave_gvl(srv_start_no_gvl, (void *)&s);

  return self;
}

static VALUE iodine_is_running(VALUE self) {
  return (facil_is_running() ? Qtrue : Qfalse);
  (void)self;
}

/* *****************************************************************************
Debug
***************************************************************************** */

/** Used for debugging purpuses. Lists GC protected objects */
VALUE iodine_print_registry(VALUE self) {
  Registry.print();
  return Qnil;
  (void)self;
}

/* *****************************************************************************
Library Initialization
***************************************************************************** */

/** Any patches required by the running environment for consistent behavior */
static void patch_env(void) {
#ifdef __APPLE__
  /* patch for dealing with the High Sierra `fork` limitations */
  void *obj_c_runtime = dlopen("Foundation.framework/Foundation", RTLD_LAZY);
  (void)obj_c_runtime;
#endif
}

/* *****************************************************************************
Ruby loads the library and invokes the Init_<lib_name> function...

Here we connect all the C code to the Ruby interface, completing the bridge
between Lib-Server and Ruby.
***************************************************************************** */
void Init_iodine(void) {
  // Set GVL for main thread
  RubyCaller.set_gvl_state(1);
  // load any environment specific patches
  patch_env();
  // initialize globally used IDs, for faster access to the Ruby layer.
  iodine_buff_var_id = rb_intern("scrtbuffer");
  iodine_call_proc_id = rb_intern("call");
  iodine_cdata_var_id = rb_intern("iodine_cdata");
  iodine_fd_var_id = rb_intern("iodine_fd");
  iodine_new_func_id = rb_intern("new");
  iodine_on_close_func_id = rb_intern("on_close");
  iodine_on_data_func_id = rb_intern("on_data");
  iodine_on_message_func_id = rb_intern("on_message");
  iodine_on_open_func_id = rb_intern("on_open");
  iodine_on_drained_func_id = rb_intern("on_drained");
  iodine_on_shutdown_func_id = rb_intern("on_shutdown");
  iodine_ping_func_id = rb_intern("ping");
  iodine_timeout_var_id = rb_intern("@timeout");
  iodine_to_i_func_id = rb_intern("to_i");
  iodine_to_s_method_id = rb_intern("to_s");

  iodine_binary_var_id = ID2SYM(rb_intern("binary"));
  iodine_channel_var_id = ID2SYM(rb_intern("channel"));
  iodine_engine_var_id = ID2SYM(rb_intern("engine"));
  iodine_force_var_id = ID2SYM(rb_intern("encoding"));
  iodine_message_var_id = ID2SYM(rb_intern("message"));
  iodine_pattern_var_id = ID2SYM(rb_intern("pattern"));
  iodine_text_var_id = ID2SYM(rb_intern("text"));

  IodineBinaryEncodingIndex = rb_enc_find_index("binary");
  IodineUTF8EncodingIndex = rb_enc_find_index("UTF-8");
  IodineBinaryEncoding = rb_enc_find("binary");
  IodineUTF8Encoding = rb_enc_find("UTF-8");

  // The core Iodine module wraps facil.io functionality and little more.
  Iodine = rb_define_module("Iodine");

  // the Iodine singleton functions
  rb_define_module_function(Iodine, "start", iodine_start, 0);
  rb_define_module_function(Iodine, "running?", iodine_is_running, 0);
  rb_define_singleton_method(Iodine, "count", iodine_count, 0);
  rb_define_module_function(Iodine, "run", iodine_run, 0);
  rb_define_module_function(Iodine, "run_after", iodine_run_after, 1);
  rb_define_module_function(Iodine, "run_every", iodine_run_every, -1);
  rb_define_module_function(Iodine, "on_idle", iodine_sched_on_idle, 0);

  /// Iodine::Base is for internal use.
  IodineBase = rb_define_module_under(Iodine, "Base");
  rb_define_module_function(IodineBase, "db_print_registry",
                            iodine_print_registry, 0);

  // Initialize the registry under the Iodine core
  Registry.init(Iodine);

  /* Initialize the rest of the library. */
  Iodine_init_protocol();
  Iodine_init_pubsub();
  Iodine_init_http();
  Iodine_init_websocket();
  Iodine_init_helpers();
}
