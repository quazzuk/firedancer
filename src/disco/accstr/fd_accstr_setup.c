#include "fd_accstr_setup.h"
#include "../../util/shmem/fd_shmem.h"

#define FD_ACCSTR_WKSP_TAG (1UL)

int
fd_accstr_wksp_create( char const * name,
                       ulong        page_sz,
                       ulong        depth,
                       ulong        mtu ) {

  if( FD_UNLIKELY( !name ) ) {
    FD_LOG_WARNING(( "NULL name" ));
    return -1;
  }

  /* Calculate required footprint */
  ulong footprint = fd_accstr_wksp_footprint( depth, mtu );
  if( FD_UNLIKELY( !footprint ) ) {
    FD_LOG_WARNING(( "invalid depth or mtu" ));
    return -1;
  }

  /* Calculate number of pages needed */
  ulong page_cnt = (footprint + page_sz - 1) / page_sz;

  /* Create the workspace */
  ulong cpu_idx = 0; /* Use CPU 0 for now */
  int err = fd_wksp_new_named( name, page_sz, 1, &page_cnt, &cpu_idx, 0660, 0, 16 );
  if( FD_UNLIKELY( err ) ) {
    FD_LOG_WARNING(( "fd_wksp_new_named failed: %d", err ));
    return -1;
  }

  /* Attach to initialize */
  fd_wksp_t * wksp = fd_wksp_attach( name );
  if( FD_UNLIKELY( !wksp ) ) {
    FD_LOG_WARNING(( "fd_wksp_attach failed" ));
    fd_wksp_delete_named( name );
    return -1;
  }

  /* Allocate space for accstr structures */
  ulong gaddr = fd_wksp_alloc( wksp, 128UL, footprint, FD_ACCSTR_WKSP_TAG );
  if( FD_UNLIKELY( !gaddr ) ) {
    FD_LOG_WARNING(( "fd_wksp_alloc failed" ));
    fd_wksp_detach( wksp );
    fd_wksp_delete_named( name );
    return -1;
  }

  void * mem = fd_wksp_laddr_fast( wksp, gaddr );

  /* Initialize accstr structures */
  if( FD_UNLIKELY( !fd_accstr_new( mem, depth, mtu, 0UL ) ) ) {
    FD_LOG_WARNING(( "fd_accstr_new failed" ));
    fd_wksp_free( wksp, gaddr );
    fd_wksp_detach( wksp );
    fd_wksp_delete_named( name );
    return -1;
  }

  fd_wksp_detach( wksp );

  FD_LOG_NOTICE(( "created accstr workspace %s (depth=%lu, mtu=%lu, footprint=%lu)",
                  name, depth, mtu, footprint ));

  return 0;
}

fd_accstr_ctx_t *
fd_accstr_wksp_attach( fd_accstr_ctx_t * ctx,
                       char const *      name ) {
  return fd_accstr_join( ctx, name );
}

void
fd_accstr_wksp_detach( fd_accstr_ctx_t * ctx ) {
  if( ctx ) {
    fd_accstr_leave( ctx );
  }
}

void
fd_accstr_wksp_destroy( char const * name ) {
  if( name ) {
    fd_wksp_delete_named( name );
    FD_LOG_NOTICE(( "destroyed accstr workspace %s", name ));
  }
}

fd_accstr_ctx_t *
fd_accstr_new_anonymous( fd_accstr_ctx_t * ctx,
                         ulong             depth,
                         ulong             mtu ) {

  if( FD_UNLIKELY( !ctx ) ) {
    FD_LOG_WARNING(( "NULL ctx" ));
    return NULL;
  }

  /* Calculate footprint */
  ulong footprint = fd_accstr_wksp_footprint( depth, mtu );
  if( FD_UNLIKELY( !footprint ) ) {
    FD_LOG_WARNING(( "invalid depth or mtu" ));
    return NULL;
  }

  /* Create anonymous workspace.
     Add extra pages for workspace metadata overhead. */
  ulong page_sz = FD_SHMEM_NORMAL_PAGE_SZ;
  ulong page_cnt = (footprint + page_sz - 1) / page_sz;
  page_cnt += 16UL; /* Extra pages for wksp overhead */

  fd_wksp_t * wksp = fd_wksp_new_anonymous( page_sz, page_cnt, 0, "accstr_anon", 0UL );
  if( FD_UNLIKELY( !wksp ) ) {
    FD_LOG_WARNING(( "fd_wksp_new_anonymous failed" ));
    return NULL;
  }

  /* Allocate space */
  ulong gaddr = fd_wksp_alloc( wksp, 128UL, footprint, FD_ACCSTR_WKSP_TAG );
  if( FD_UNLIKELY( !gaddr ) ) {
    FD_LOG_WARNING(( "fd_wksp_alloc failed" ));
    fd_wksp_delete_anonymous( wksp );
    return NULL;
  }

  void * mem = fd_wksp_laddr_fast( wksp, gaddr );

  /* Initialize accstr structures */
  if( FD_UNLIKELY( !fd_accstr_new( mem, depth, mtu, 0UL ) ) ) {
    FD_LOG_WARNING(( "fd_accstr_new failed" ));
    fd_wksp_delete_anonymous( wksp );
    return NULL;
  }

  /* Initialize context directly (no join needed for anonymous) */
  fd_memset( ctx, 0, sizeof(fd_accstr_ctx_t) );

  fd_accstr_shmem_hdr_t * hdr = (fd_accstr_shmem_hdr_t *)mem;

  ctx->wksp   = wksp;
  ctx->hdr    = hdr;
  ctx->mcache = fd_mcache_join( fd_wksp_laddr_fast( wksp, hdr->mcache_gaddr ) );
  ctx->dcache = fd_dcache_join( fd_wksp_laddr_fast( wksp, hdr->dcache_gaddr ) );
  ctx->fseq   = fd_fseq_join( fd_wksp_laddr_fast( wksp, hdr->fseq_gaddr ) );
  ctx->depth  = hdr->depth;
  ctx->seq    = hdr->seq0;
  ctx->mseq   = fd_mcache_seq_laddr( ctx->mcache );
  ctx->mtu    = hdr->mtu;
  ctx->chunk0 = fd_dcache_compact_chunk0( wksp, ctx->dcache );
  ctx->wmark  = fd_dcache_compact_wmark( wksp, ctx->dcache, hdr->mtu );
  ctx->chunk  = ctx->chunk0;

  fd_accstr_filter_init( ctx->filter );
  fd_accstr_filter_set_pass_all( ctx->filter );

  ctx->magic = FD_ACCSTR_CTX_MAGIC;

  FD_LOG_NOTICE(( "created anonymous accstr (depth=%lu, mtu=%lu)", depth, mtu ));

  return ctx;
}

void
fd_accstr_delete_anonymous( fd_accstr_ctx_t * ctx ) {
  if( !ctx || ctx->magic != FD_ACCSTR_CTX_MAGIC ) {
    return;
  }

  fd_wksp_t * wksp = ctx->wksp;

  if( ctx->fseq )   fd_fseq_leave( ctx->fseq );
  if( ctx->dcache ) fd_dcache_leave( ctx->dcache );
  if( ctx->mcache ) fd_mcache_leave( ctx->mcache );

  ctx->magic = 0;

  if( wksp ) {
    fd_wksp_delete_anonymous( wksp );
  }

  FD_LOG_NOTICE(( "deleted anonymous accstr" ));
}
