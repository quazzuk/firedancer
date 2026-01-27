/* Wrapper header for bindgen - includes all firedancer headers needed
   for the accstr consumer Rust bindings.

   Note: These paths are relative to the firedancer source tree.
   The build.rs adds -I flags for the firedancer root and src/ directories. */

#include "src/disco/accstr/fd_accstr_proto.h"
#include "src/disco/accstr/fd_accstr_consumer.h"
#include "src/util/wksp/fd_wksp.h"
#include "src/tango/mcache/fd_mcache.h"
#include "src/tango/dcache/fd_dcache.h"
#include "src/tango/fseq/fd_fseq.h"
