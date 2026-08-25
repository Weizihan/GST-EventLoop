#pragma once

#ifdef GST_EVENTLOOP_USE_GST_LOG
#include "GstLog.h"

#define GST_EVENTLOOP_LOG_DEBUG(...) DEBUG("[GST_EventLoop] " __VA_ARGS__)
#define GST_EVENTLOOP_LOG_INFO(...)  INFO("[GST_EventLoop] " __VA_ARGS__)
#define GST_EVENTLOOP_LOG_WARN(...)  WARN("[GST_EventLoop] " __VA_ARGS__)
#define GST_EVENTLOOP_LOG_ERROR(...) ERROR("[GST_EventLoop] " __VA_ARGS__)

#else

#define GST_EVENTLOOP_LOG_DEBUG(...) do { } while (false)
#define GST_EVENTLOOP_LOG_INFO(...)  do { } while (false)
#define GST_EVENTLOOP_LOG_WARN(...)  do { } while (false)
#define GST_EVENTLOOP_LOG_ERROR(...) do { } while (false)

#endif
