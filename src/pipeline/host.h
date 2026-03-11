#ifndef P2P_HOST_H
#define P2P_HOST_H

#include "pipeline.h"

/* Build GStreamer pipeline description string for HOST role.
 * Returns a pointer to a static buffer — copy before next call. */
const char *build_host_pipeline_str(const StreamConfig *cfg);

#endif
