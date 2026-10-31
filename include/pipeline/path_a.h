/*
 * path_a.h -- Path A processing module for A5-A11 devices.
 *
 * Chains: checkm8 DFU handler -> ramdisk -> kernel modification ->
 * activation record submission -> deletescript cleanup -> verify.
 */

#ifndef PATH_A_H
#define PATH_A_H

#include "pipeline/pipeline.h"

/* The Path A processing module for registration with pipeline_register(). */
extern const pipeline_module_t path_a_module;

#endif /* PATH_A_H */
