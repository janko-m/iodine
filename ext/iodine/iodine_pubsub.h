#ifndef H_IODINE_ENGINE_H
#define H_IODINE_ENGINE_H
/*
Copyright: Boaz segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#include "iodine.h"
#include "pubsub.h"

typedef enum {
  IODINE_PUBSUB_GLOBAL,
  IODINE_PUBSUB_WEBSOCKET,
  IODINE_PUBSUB_SSE
} iodine_pubsub_type_e;

extern VALUE IodineEngine;
extern ID iodine_engine_pubid;
VALUE iodine_publish(int argc, VALUE *argv, VALUE self);
VALUE iodine_subscribe(int argc, VALUE *argv, void *owner,
                       iodine_pubsub_type_e type);

typedef struct {
  pubsub_engine_s engine;
  pubsub_engine_s *p;
  VALUE handler;
  void (*dealloc)(pubsub_engine_s *);
} iodine_engine_s;

void Iodine_init_pubsub(void);

typedef struct pubsub_engine_s pubsub_engine_s;

pubsub_engine_s *iodine_engine_ruby2facil(VALUE engine);

#endif
