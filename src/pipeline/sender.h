#ifndef P2P_SENDER_H
#define P2P_SENDER_H

#include "pipeline.h"

/* Build GStreamer pipeline description string for SENDER role.
 * Returns a pointer to a static buffer — copy before next call. */
const char *build_sender_pipeline_str(const StreamConfig *cfg);

#endif /* P2P_SENDER_H */
