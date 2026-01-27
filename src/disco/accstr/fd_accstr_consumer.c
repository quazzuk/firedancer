#include "fd_accstr_consumer.h"
#include "../../util/log/fd_log.h"

fd_accstr_consumer_t *
fd_accstr_consumer_attach( fd_accstr_consumer_t * consumer,
                           char const *           wksp_name ) {

  if( FD_UNLIKELY( !consumer ) ) {
    FD_LOG_WARNING(( "NULL consumer" ));
    return NULL;
  }

  if( FD_UNLIKELY( !wksp_name ) ) {
    FD_LOG_WARNING(( "NULL wksp_name" ));
    return NULL;
  }

  /* Initialize consumer to known state */
  fd_memset( consumer, 0, sizeof(fd_accstr_consumer_t) );

  /* Attach to workspace */
  fd_wksp_t * wksp = fd_wksp_attach( wksp_name );
  if( FD_UNLIKELY( !wksp ) ) {
    FD_LOG_WARNING(( "failed to attach to workspace %s", wksp_name ));
    return NULL;
  }

  /* Find the accstr header using tag query */
  ulong tag = 1UL; /* Assume accstr uses tag 1 */
  fd_wksp_tag_query_info_t info[1];
  if( FD_UNLIKELY( fd_wksp_tag_query( wksp, &tag, 1UL, info, 1UL ) != 1UL ) ) {
    FD_LOG_WARNING(( "failed to find accstr header in workspace %s", wksp_name ));
    fd_wksp_detach( wksp );
    return NULL;
  }

  fd_accstr_shmem_hdr_t * hdr = (fd_accstr_shmem_hdr_t *)fd_wksp_laddr_fast( wksp, info->gaddr_lo );

  /* Validate header */
  if( FD_UNLIKELY( hdr->magic != FD_ACCSTR_SHMEM_MAGIC ) ) {
    FD_LOG_WARNING(( "invalid accstr magic in workspace %s", wksp_name ));
    fd_wksp_detach( wksp );
    return NULL;
  }

  if( FD_UNLIKELY( hdr->version != FD_ACCSTR_SHMEM_VERSION ) ) {
    FD_LOG_WARNING(( "version mismatch in workspace %s: expected %lu, got %lu",
                     wksp_name, FD_ACCSTR_SHMEM_VERSION, hdr->version ));
    fd_wksp_detach( wksp );
    return NULL;
  }

  /* Join mcache (read-only for consumer) */
  fd_frag_meta_t * mcache = fd_mcache_join( fd_wksp_laddr_fast( wksp, hdr->mcache_gaddr ) );
  if( FD_UNLIKELY( !mcache ) ) {
    FD_LOG_WARNING(( "failed to join mcache" ));
    fd_wksp_detach( wksp );
    return NULL;
  }

  /* Join dcache (read-only for consumer) */
  uchar * dcache = fd_dcache_join( fd_wksp_laddr_fast( wksp, hdr->dcache_gaddr ) );
  if( FD_UNLIKELY( !dcache ) ) {
    FD_LOG_WARNING(( "failed to join dcache" ));
    fd_mcache_leave( mcache );
    fd_wksp_detach( wksp );
    return NULL;
  }

  /* Join fseq (read-write for flow control) */
  ulong * fseq = fd_fseq_join( fd_wksp_laddr_fast( wksp, hdr->fseq_gaddr ) );
  if( FD_UNLIKELY( !fseq ) ) {
    FD_LOG_WARNING(( "failed to join fseq" ));
    fd_dcache_leave( dcache );
    fd_mcache_leave( mcache );
    fd_wksp_detach( wksp );
    return NULL;
  }

  /* Initialize consumer */
  consumer->wksp   = wksp;
  consumer->hdr    = hdr;
  consumer->mcache = mcache;
  consumer->dcache = dcache;
  consumer->fseq   = fseq;
  consumer->depth  = hdr->depth;
  consumer->seq    = hdr->seq0;

  consumer->received_cnt = 0;
  consumer->overrun_cnt  = 0;

  /* Initialize flow control to starting sequence */
  fd_fseq_update( fseq, consumer->seq );

  FD_LOG_NOTICE(( "attached to accstr workspace %s (depth=%lu, mtu=%lu, seq0=%lu)",
                  wksp_name, hdr->depth, hdr->mtu, hdr->seq0 ));

  return consumer;
}

void *
fd_accstr_consumer_detach( fd_accstr_consumer_t * consumer ) {

  if( FD_UNLIKELY( !consumer ) ) {
    FD_LOG_WARNING(( "NULL consumer" ));
    return NULL;
  }

  FD_LOG_NOTICE(( "detaching from accstr (received=%lu, overruns=%lu)",
                  consumer->received_cnt, consumer->overrun_cnt ));

  /* Leave structures */
  if( consumer->fseq )   fd_fseq_leave( consumer->fseq );
  if( consumer->dcache ) fd_dcache_leave( (void *)consumer->dcache );
  if( consumer->mcache ) fd_mcache_leave( (void *)consumer->mcache );
  if( consumer->wksp )   fd_wksp_detach( consumer->wksp );

  /* Clear consumer state */
  fd_memset( consumer, 0, sizeof(fd_accstr_consumer_t) );

  return consumer;
}
