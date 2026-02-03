#ifndef HEADER_fd_src_disco_accstr_fd_accstr_setup_h
#define HEADER_fd_src_disco_accstr_fd_accstr_setup_h

/* fd_accstr_setup.h provides helper functions for setting up the
   account stream infrastructure in firedancer.

   There are two setup patterns:

   1. External workspace (recommended for production):
      - Create a named workspace that external consumers can attach to
      - Use fd_accstr_wksp_create() and fd_accstr_wksp_attach()

   2. Anonymous workspace (useful for testing):
      - Create an anonymous workspace in the current process
      - Use fd_accstr_new_anonymous()

   Example (production setup):

     // At startup, create the workspace
     fd_accstr_wksp_create( "fd1_accstr", FD_SHMEM_HUGE_PAGE_SZ,
                            FD_ACCSTR_DEFAULT_DEPTH, FD_ACCSTR_DEFAULT_MTU );

     // In exec tile init, join the workspace
     fd_accstr_ctx_t accstr_ctx[1];
     fd_accstr_ctx_t * ctx = fd_accstr_wksp_attach( accstr_ctx, "fd1_accstr" );

     // Configure owner filter
     fd_accstr_filter_add_owner( ctx->filter, token_program_id );
     fd_accstr_filter_add_owner( ctx->filter, my_program_id );

     // Set in runtime
     runtime->log.accstr_ctx = ctx;

     // External consumers can now attach via fd_accstr_consumer_attach()
*/

#include "fd_accstr_ctx.h"

FD_PROTOTYPES_BEGIN

/* fd_accstr_wksp_create creates a new named shared memory workspace
   for the account stream. This should be called once at startup
   (typically in the main process before forking tiles).

   name is the workspace name (e.g., "fd1_accstr").
   page_sz is the huge page size to use (e.g., FD_SHMEM_HUGE_PAGE_SZ).
   depth is the mcache depth (must be power of 2, default 32768).
   mtu is the max message size (default 10MB + header).

   Returns 0 on success, -1 on failure. */

int
fd_accstr_wksp_create( char const * name,
                       ulong        page_sz,
                       ulong        depth,
                       ulong        mtu );

/* fd_accstr_wksp_attach attaches a context to an existing accstr
   workspace. This should be called in each tile that needs to
   publish account updates (typically the exec tile).

   ctx is a pointer to an fd_accstr_ctx_t (can be on stack or in scratch).
   name is the workspace name to attach to.

   Returns ctx on success, NULL on failure. */

fd_accstr_ctx_t *
fd_accstr_wksp_attach( fd_accstr_ctx_t * ctx,
                       char const *      name );

/* fd_accstr_wksp_detach detaches from the workspace.
   Call this during tile shutdown. */

void
fd_accstr_wksp_detach( fd_accstr_ctx_t * ctx );

/* fd_accstr_wksp_destroy destroys the named workspace.
   Call this during main process shutdown. */

void
fd_accstr_wksp_destroy( char const * name );

/* fd_accstr_new_anonymous creates an anonymous (in-process) accstr
   workspace. Useful for testing. The workspace is not accessible
   from other processes.

   ctx is a pointer to an fd_accstr_ctx_t.
   depth and mtu are the configuration parameters.

   Returns ctx on success, NULL on failure. */

fd_accstr_ctx_t *
fd_accstr_new_anonymous( fd_accstr_ctx_t * ctx,
                         ulong             depth,
                         ulong             mtu );

/* fd_accstr_delete_anonymous cleans up an anonymous workspace. */

void
fd_accstr_delete_anonymous( fd_accstr_ctx_t * ctx );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_disco_accstr_fd_accstr_setup_h */
