/* test_accstr.c - Unit test for account streaming functionality */

#include "fd_accstr_setup.h"
#include "fd_accstr_consumer.h"
#include "../../util/fd_util.h"

/* Test account data */
static uchar test_pubkey[32] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                                  17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32 };
static uchar test_owner[32]  = { 0x06,0xdd,0xf6,0xe1,0xd7,0x65,0xa1,0x93,
                                  0xd9,0xcb,0xe1,0x46,0xce,0xeb,0x79,0xac,
                                  0x1c,0xb4,0x85,0xed,0x5f,0x5b,0x37,0x91,
                                  0x3a,0x8c,0xf5,0x85,0x7e,0xff,0x00,0xa9 }; /* Token program */

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  FD_LOG_NOTICE(( "Testing accstr functionality" ));

  /* Create anonymous workspace for testing */
  fd_accstr_ctx_t producer_ctx[1];
  ulong depth = 64UL;
  ulong mtu   = 4096UL; /* 4KB for testing - small accounts only */

  fd_accstr_ctx_t * producer = fd_accstr_new_anonymous( producer_ctx, depth, mtu );
  if( FD_UNLIKELY( !producer ) ) {
    FD_LOG_ERR(( "FAIL: fd_accstr_new_anonymous returned NULL" ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Created anonymous accstr workspace" ));

  /* Configure filter to pass the test owner */
  fd_accstr_filter_init( producer->filter );
  fd_accstr_filter_add_owner( producer->filter, test_owner );
  FD_LOG_NOTICE(( "PASS: Configured owner filter" ));

  /* Publish some test account updates */
  uchar test_data[256];
  for( ulong i=0; i<256; i++ ) test_data[i] = (uchar)i;

  int published = fd_accstr_publish( producer,
                                      test_pubkey,
                                      test_owner,
                                      1000000UL,    /* lamports */
                                      0,            /* not executable */
                                      0UL,          /* rent_epoch */
                                      12345UL,      /* slot */
                                      test_data,
                                      256UL );
  if( !published ) {
    FD_LOG_ERR(( "FAIL: fd_accstr_publish returned 0" ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Published account update" ));

  /* Publish with wrong owner (should be filtered) */
  uchar wrong_owner[32] = {0};
  int filtered = fd_accstr_publish( producer,
                                     test_pubkey,
                                     wrong_owner,
                                     1000000UL,
                                     0,
                                     0UL,
                                     12345UL,
                                     test_data,
                                     256UL );
  if( filtered ) {
    FD_LOG_ERR(( "FAIL: Account with wrong owner should have been filtered" ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Correctly filtered account with wrong owner" ));

  /* Now consume from the producer's mcache/dcache directly
     (simulating what an external consumer would do) */

  /* Read from mcache */
  ulong const * seq_ptr = fd_mcache_seq_laddr_const( producer->mcache );
  ulong tx_seq = fd_mcache_seq_query( seq_ptr );
  ulong expected_seq = producer->hdr->seq0;

  long diff = fd_seq_diff( tx_seq, expected_seq );
  if( diff != 1L ) {
    FD_LOG_ERR(( "FAIL: Expected 1 message, got diff=%ld", diff ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: mcache shows 1 message published" ));

  /* Read the message */
  ulong idx = fd_mcache_line_idx( expected_seq, depth );
  fd_frag_meta_t const * mline = producer->mcache + idx;

  if( mline->seq != expected_seq ) {
    FD_LOG_ERR(( "FAIL: Sequence mismatch in mcache" ));
    return 1;
  }

  ulong msg_type = FD_ACCSTR_SIG_TYPE( mline->sig );
  if( msg_type != FD_ACCSTR_MSG_ACCOUNT_UPDATE ) {
    FD_LOG_ERR(( "FAIL: Wrong message type %lu", msg_type ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Message type is ACCOUNT_UPDATE" ));

  /* Read payload */
  uchar const * payload = (uchar const *)fd_chunk_to_laddr_const( producer->wksp, mline->chunk );
  fd_accstr_account_update_t const * update = (fd_accstr_account_update_t const *)payload;

  /* Verify fields */
  if( memcmp( update->pubkey, test_pubkey, 32 ) != 0 ) {
    FD_LOG_ERR(( "FAIL: Pubkey mismatch" ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Pubkey matches" ));

  if( memcmp( update->owner, test_owner, 32 ) != 0 ) {
    FD_LOG_ERR(( "FAIL: Owner mismatch" ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Owner matches" ));

  if( update->lamports != 1000000UL ) {
    FD_LOG_ERR(( "FAIL: Lamports mismatch: got %lu", update->lamports ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Lamports match" ));

  if( update->slot != 12345UL ) {
    FD_LOG_ERR(( "FAIL: Slot mismatch: got %lu", update->slot ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Slot matches" ));

  if( update->data_len != 256UL ) {
    FD_LOG_ERR(( "FAIL: Data length mismatch: got %lu", update->data_len ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Data length matches" ));

  /* Verify data content */
  uchar const * data = payload + FD_ACCSTR_ACCOUNT_UPDATE_FIXED_SZ;
  for( ulong i=0; i<256; i++ ) {
    if( data[i] != (uchar)i ) {
      FD_LOG_ERR(( "FAIL: Data byte %lu mismatch: got %u, expected %lu", i, data[i], i ));
      return 1;
    }
  }
  FD_LOG_NOTICE(( "PASS: Account data matches" ));

  /* Check stats */
  if( producer->published_cnt != 1UL ) {
    FD_LOG_ERR(( "FAIL: published_cnt=%lu, expected 1", producer->published_cnt ));
    return 1;
  }
  if( producer->filtered_cnt != 1UL ) {
    FD_LOG_ERR(( "FAIL: filtered_cnt=%lu, expected 1", producer->filtered_cnt ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Statistics correct (published=1, filtered=1)" ));

  /* Test transaction boundary */
  uchar test_signature[64];
  for( ulong i=0; i<64; i++ ) test_signature[i] = (uchar)(i + 0x40);

  fd_accstr_publish_txn_boundary( producer,
                                   12345UL,   /* slot */
                                   42UL,      /* txn_idx */
                                   test_signature,
                                   1UL,       /* account_count */
                                   1 );       /* success */
  FD_LOG_NOTICE(( "PASS: Published transaction boundary" ));

  /* Verify transaction boundary message */
  ulong txn_boundary_seq = producer->hdr->seq0 + 1; /* After account update */
  ulong txn_boundary_idx = fd_mcache_line_idx( txn_boundary_seq, depth );
  fd_frag_meta_t const * txn_mline = producer->mcache + txn_boundary_idx;

  if( txn_mline->seq != txn_boundary_seq ) {
    FD_LOG_ERR(( "FAIL: Transaction boundary sequence mismatch" ));
    return 1;
  }

  ulong txn_msg_type = FD_ACCSTR_SIG_TYPE( txn_mline->sig );
  if( txn_msg_type != FD_ACCSTR_MSG_TXN_BOUNDARY ) {
    FD_LOG_ERR(( "FAIL: Wrong message type for txn boundary: %lu", txn_msg_type ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Transaction boundary message type correct" ));

  /* Verify transaction boundary fields */
  uchar const * txn_payload = (uchar const *)fd_chunk_to_laddr_const( producer->wksp, txn_mline->chunk );
  fd_accstr_txn_boundary_t const * txn_boundary = (fd_accstr_txn_boundary_t const *)txn_payload;

  if( txn_boundary->slot != 12345UL ) {
    FD_LOG_ERR(( "FAIL: Txn boundary slot mismatch: got %lu", txn_boundary->slot ));
    return 1;
  }
  if( txn_boundary->txn_idx != 42UL ) {
    FD_LOG_ERR(( "FAIL: Txn boundary txn_idx mismatch: got %lu", txn_boundary->txn_idx ));
    return 1;
  }
  if( memcmp( txn_boundary->signature, test_signature, 64 ) != 0 ) {
    FD_LOG_ERR(( "FAIL: Txn boundary signature mismatch" ));
    return 1;
  }
  if( txn_boundary->account_count != 1UL ) {
    FD_LOG_ERR(( "FAIL: Txn boundary account_count mismatch: got %lu", txn_boundary->account_count ));
    return 1;
  }
  if( txn_boundary->success != 1 ) {
    FD_LOG_ERR(( "FAIL: Txn boundary success mismatch: got %d", txn_boundary->success ));
    return 1;
  }
  FD_LOG_NOTICE(( "PASS: Transaction boundary fields verified" ));

  /* Test slot boundary */
  fd_accstr_publish_slot_boundary( producer, 12345UL, 1UL );
  FD_LOG_NOTICE(( "PASS: Published slot boundary" ));

  /* Cleanup */
  fd_accstr_delete_anonymous( producer );
  FD_LOG_NOTICE(( "PASS: Cleaned up anonymous workspace" ));

  FD_LOG_NOTICE(( "All tests passed!" ));

  fd_halt();
  return 0;
}
