/* fd_accstr_wksp_create.c creates the accstr workspace for firedancer.

   This utility should be run once before starting firedancer to set up
   the shared memory workspace that exec tiles will use to publish
   account updates.

   Usage:
     fd_accstr_wksp_create <workspace_name> [depth] [mtu]

   Example:
     fd_accstr_wksp_create fd1_accstr

   Default parameters:
     depth: 32768 (must be power of 2)
     mtu:   10485760 (10MB + header, max account size)

   The workspace will be created with mode 0660 and can be attached to
   by processes in the same group. */

#include "fd_accstr_setup.h"
#include "../../util/fd_util.h"

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  if( argc < 2 ) {
    FD_LOG_ERR(( "Usage: %s <workspace_name> [depth] [mtu]", argv[0] ));
    return 1;
  }

  char const * wksp_name = argv[1];
  ulong depth = FD_ACCSTR_DEFAULT_DEPTH;
  ulong mtu   = FD_ACCSTR_DEFAULT_MTU;

  if( argc > 2 ) {
    depth = fd_cstr_to_ulong( argv[2] );
  }
  if( argc > 3 ) {
    mtu = fd_cstr_to_ulong( argv[3] );
  }

  FD_LOG_NOTICE(( "Creating accstr workspace:" ));
  FD_LOG_NOTICE(( "  Name:  %s", wksp_name ));
  FD_LOG_NOTICE(( "  Depth: %lu", depth ));
  FD_LOG_NOTICE(( "  MTU:   %lu", mtu ));

  int err = fd_accstr_wksp_create( wksp_name, FD_SHMEM_HUGE_PAGE_SZ, depth, mtu );

  if( err ) {
    FD_LOG_ERR(( "Failed to create workspace" ));
    return 1;
  }

  FD_LOG_NOTICE(( "Workspace created successfully" ));
  FD_LOG_NOTICE(( "  Exec tiles will auto-attach to %s_accstr", wksp_name ));
  FD_LOG_NOTICE(( "  Consumers can attach with: fd_accstr_example %s", wksp_name ));

  fd_halt();
  return 0;
}
