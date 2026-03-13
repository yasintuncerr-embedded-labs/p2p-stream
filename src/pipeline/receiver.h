#ifndef P2P_RECEIVER_H
#define P2P_RECEIVER_H

#include "pipeline.h"

/* Build Gstreamer pipeline description strings for RECEIVER role.
 * Returns a pointer to a static buffer - copy next call */
const char *build_receiver_pipeline_str(const StreamConfig *cfg);

#endif /* P2P_RECEIVER_H */
