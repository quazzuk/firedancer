#include "fd_accstr_ctx.h"
#include "../../util/log/fd_log.h"

void *
fd_accstr_new( void * mem,
               ulong  depth,
               ulong  mtu,
               ulong  seq0 ) {

  if( FD_UNLIKELY( !mem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)mem, 128UL ) ) ) {
    FD_LOG_WARNING(( "misaligned mem" ));
    return NULL;
  }

  /* Validate depth is power of 2 */
  if( FD_UNLIKELY( !fd_ulong_is_pow2( depth ) ) ) {
    FD_LOG_WARNING(( "depth must be power of 2" ));
    return NULL;
  }

  FD_SCRATCH_ALLOC_INIT( l, mem );

  /* Allocate header */
  fd_accstr_shmem_hdr_t * hdr = FD_SCRATCH_ALLOC_APPEND( l, 128UL, sizeof(fd_accstr_shmem_hdr_t) );
  fd_memset( hdr, 0, sizeof(fd_accstr_shmem_hdr_t) );

  /* Allocate and initialize mcache */
  void * mcache_mem = FD_SCRATCH_ALLOC_APPEND( l, fd_mcache_align(), fd_mcache_footprint( depth, 0 ) );
  fd_mcache_new( mcache_mem, depth, 0, seq0 );

  /* Allocate and initialize dcache */
  ulong dcache_data_sz = fd_dcache_req_data_sz( mtu, depth, 1, 1 );
  void * dcache_mem = FD_SCRATCH_ALLOC_APPEND( l, fd_dcache_align(), dcache_data_sz );
  fd_dcache_new( dcache_mem, dcache_data_sz, 0 );

  /* Allocate and initialize fseq */
  void * fseq_mem = FD_SCRATCH_ALLOC_APPEND( l, fd_fseq_align(), fd_fseq_footprint() );
  fd_fseq_new( fseq_mem, seq0 );

  FD_SCRATCH_ALLOC_FINI( l, 128UL );

  /* Store global addresses in header */
  fd_wksp_t * wksp = fd_wksp_containing( mem );
  if( FD_UNLIKELY( !wksp ) ) {
    FD_LOG_WARNING(( "mem not in a workspace" ));
    return NULL;
  }

  hdr->magic        = FD_ACCSTR_SHMEM_MAGIC;
  hdr->version      = FD_ACCSTR_SHMEM_VERSION;
  hdr->mcache_gaddr = fd_wksp_gaddr_fast( wksp, mcache_mem );
  hdr->dcache_gaddr = fd_wksp_gaddr_fast( wksp, dcache_mem );
  hdr->fseq_gaddr   = fd_wksp_gaddr_fast( wksp, fseq_mem );
  hdr->depth        = depth;
  hdr->mtu          = mtu;
  hdr->seq0         = seq0;

  FD_COMPILER_MFENCE();

  return mem;
}

fd_accstr_ctx_t *
fd_accstr_join( fd_accstr_ctx_t * ctx,
                char const *      wksp_name ) {

  if( FD_UNLIKELY( !ctx ) ) {
    FD_LOG_WARNING(( "NULL ctx" ));
    return NULL;
  }

  if( FD_UNLIKELY( !wksp_name ) ) {
    FD_LOG_WARNING(( "NULL wksp_name" ));
    return NULL;
  }

  /* Attach to workspace */
  fd_wksp_t * wksp = fd_wksp_attach( wksp_name );
  if( FD_UNLIKELY( !wksp ) ) {
    FD_LOG_WARNING(( "failed to attach to workspace %s", wksp_name ));
    return NULL;
  }

  /* Find the header (should be at known offset in workspace) */
  /* For now, assume it's at the start of the first allocation */
  fd_accstr_shmem_hdr_t * hdr = NULL;

  /* Iterate through partitions to find our header */
  fd_wksp_usage_t usage[1];
  fd_wksp_usage( wksp, NULL, 0UL, usage );

  /* Use tag query to find our allocation */
  ulong tag = 1UL; /* Assume we use tag 1 for accstr */
  fd_wksp_tag_query_info_t info[1];
  if( fd_wksp_tag_query( wksp, &tag, 1UL, info, 1UL ) == 1UL ) {
    hdr = (fd_accstr_shmem_hdr_t *)fd_wksp_laddr_fast( wksp, info->gaddr_lo );
  }

  if( FD_UNLIKELY( !hdr || hdr->magic != FD_ACCSTR_SHMEM_MAGIC ) ) {
    FD_LOG_WARNING(( "invalid accstr header in workspace %s", wksp_name ));
    fd_wksp_detach( wksp );
    return NULL;
  }

  if( FD_UNLIKELY( hdr->version != FD_ACCSTR_SHMEM_VERSION ) ) {
    FD_LOG_WARNING(( "version mismatch: expected %lu, got %lu", FD_ACCSTR_SHMEM_VERSION, hdr->version ));
    fd_wksp_detach( wksp );
    return NULL;
  }

  /* Join mcache */
  fd_frag_meta_t * mcache = fd_mcache_join( fd_wksp_laddr_fast( wksp, hdr->mcache_gaddr ) );
  if( FD_UNLIKELY( !mcache ) ) {
    FD_LOG_WARNING(( "failed to join mcache" ));
    fd_wksp_detach( wksp );
    return NULL;
  }

  /* Join dcache */
  uchar * dcache = fd_dcache_join( fd_wksp_laddr_fast( wksp, hdr->dcache_gaddr ) );
  if( FD_UNLIKELY( !dcache ) ) {
    FD_LOG_WARNING(( "failed to join dcache" ));
    fd_mcache_leave( mcache );
    fd_wksp_detach( wksp );
    return NULL;
  }

  /* Join fseq */
  ulong * fseq = fd_fseq_join( fd_wksp_laddr_fast( wksp, hdr->fseq_gaddr ) );
  if( FD_UNLIKELY( !fseq ) ) {
    FD_LOG_WARNING(( "failed to join fseq" ));
    fd_dcache_leave( dcache );
    fd_mcache_leave( mcache );
    fd_wksp_detach( wksp );
    return NULL;
  }

  /* Initialize context */
  fd_memset( ctx, 0, sizeof(fd_accstr_ctx_t) );

  ctx->wksp   = wksp;
  ctx->hdr    = hdr;
  ctx->mcache = mcache;
  ctx->dcache = dcache;
  ctx->fseq   = fseq;
  ctx->depth  = hdr->depth;
  ctx->seq    = hdr->seq0;
  ctx->mseq   = fd_mcache_seq_laddr( mcache );
  ctx->mtu    = hdr->mtu;

  /* Compute dcache chunk bounds */
  ctx->chunk0 = fd_dcache_compact_chunk0( wksp, dcache );
  ctx->wmark  = fd_dcache_compact_wmark( wksp, dcache, hdr->mtu );
  ctx->chunk  = ctx->chunk0;

  /* Initialize filter to pass-all by default */
  fd_accstr_filter_init( ctx->filter );
  fd_accstr_filter_set_pass_all( ctx->filter );

  /* Initialize write version */
  ctx->write_version = 0;

  /* Initialize stats */
  ctx->published_cnt = 0;
  ctx->filtered_cnt  = 0;
  ctx->overrun_cnt   = 0;

  FD_COMPILER_MFENCE();
  ctx->magic = FD_ACCSTR_CTX_MAGIC;
  FD_COMPILER_MFENCE();

  FD_LOG_NOTICE(( "joined accstr workspace %s (depth=%lu, mtu=%lu)", wksp_name, hdr->depth, hdr->mtu ));

  return ctx;
}

void *
fd_accstr_leave( fd_accstr_ctx_t * ctx ) {

  if( FD_UNLIKELY( !ctx ) ) {
    FD_LOG_WARNING(( "NULL ctx" ));
    return NULL;
  }

  if( FD_UNLIKELY( ctx->magic != FD_ACCSTR_CTX_MAGIC ) ) {
    FD_LOG_WARNING(( "bad magic" ));
    return NULL;
  }

  FD_LOG_NOTICE(( "leaving accstr (published=%lu, filtered=%lu, overruns=%lu)",
                  ctx->published_cnt, ctx->filtered_cnt, ctx->overrun_cnt ));

  /* Leave structures */
  if( ctx->fseq )   fd_fseq_leave( ctx->fseq );
  if( ctx->dcache ) fd_dcache_leave( ctx->dcache );
  if( ctx->mcache ) fd_mcache_leave( ctx->mcache );
  if( ctx->wksp )   fd_wksp_detach( ctx->wksp );

  FD_COMPILER_MFENCE();
  ctx->magic = 0UL;
  FD_COMPILER_MFENCE();

  return ctx;
}
