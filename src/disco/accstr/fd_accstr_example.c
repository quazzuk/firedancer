/* fd_accstr_example.c demonstrates how to consume account updates
   from firedancer via the accstr shared memory channel.

   Usage:
     fd_accstr_example <workspace_name>

   The program will attach to the accstr workspace and print account
   updates as they are received. Press Ctrl+C to exit. */

#include "fd_accstr_consumer.h"
#include "../../util/fd_util.h"
#include "../../ballet/base58/fd_base58.h"

#include <signal.h>

static volatile int g_running = 1;

static void
signal_handler( int sig ) {
  (void)sig;
  g_running = 0;
}

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  if( argc < 2 ) {
    FD_LOG_ERR(( "Usage: %s <workspace_name>", argv[0] ));
    return 1;
  }

  char const * wksp_name = argv[1];

  /* Set up signal handler for clean shutdown */
  signal( SIGINT, signal_handler );
  signal( SIGTERM, signal_handler );

  /* Attach to accstr workspace */
  fd_accstr_consumer_t consumer[1];
  if( !fd_accstr_consumer_attach( consumer, wksp_name ) ) {
    FD_LOG_ERR(( "Failed to attach to workspace %s", wksp_name ));
    return 1;
  }

  FD_LOG_NOTICE(( "Attached to accstr workspace %s, waiting for updates...", wksp_name ));

  ulong update_cnt = 0;
  ulong slot_cnt = 0;

  while( g_running ) {
    /* Poll for account updates */
    fd_accstr_account_update_t const * update;
    uchar const * data;

    int rc = fd_accstr_consumer_poll( consumer, &update, &data );

    if( rc == 1 ) {
      /* Got an account update */
      char pubkey_b58[ FD_BASE58_ENCODED_32_SZ ];
      char owner_b58[ FD_BASE58_ENCODED_32_SZ ];
      fd_base58_encode_32( update->pubkey, NULL, pubkey_b58 );
      fd_base58_encode_32( update->owner, NULL, owner_b58 );

      FD_LOG_NOTICE(( "Account update #%lu:", update_cnt ));
      FD_LOG_NOTICE(( "  Pubkey:    %s", pubkey_b58 ));
      FD_LOG_NOTICE(( "  Owner:     %s", owner_b58 ));
      FD_LOG_NOTICE(( "  Slot:      %lu", update->slot ));
      FD_LOG_NOTICE(( "  Lamports:  %lu", update->lamports ));
      FD_LOG_NOTICE(( "  Data len:  %lu", update->data_len ));
      FD_LOG_NOTICE(( "  Exec:      %s", update->executable ? "yes" : "no" ));
      FD_LOG_NOTICE(( "  Version:   %lu", update->write_version ));

      fd_accstr_consumer_ack( consumer );
      update_cnt++;

    } else if( rc == 0 ) {
      /* No data available - check for slot boundaries */
      fd_accstr_slot_boundary_t const * boundary;
      rc = fd_accstr_consumer_poll_slot_boundary( consumer, &boundary );

      if( rc == 1 ) {
        FD_LOG_NOTICE(( "Slot %lu completed: %lu accounts updated",
                        boundary->slot, boundary->account_count ));
        fd_accstr_consumer_ack( consumer );
        slot_cnt++;
      } else {
        /* Nothing available, yield CPU */
        fd_log_sleep( 1000 ); /* 1ms */
      }

    } else if( rc == -1 ) {
      /* Consumer overrun */
      ulong received, overruns;
      fd_accstr_consumer_get_stats( consumer, &received, &overruns );
      FD_LOG_WARNING(( "Consumer overrun! Total overruns: %lu (received: %lu)",
                       overruns, received ));
    }
  }

  /* Print final stats */
  ulong received, overruns;
  fd_accstr_consumer_get_stats( consumer, &received, &overruns );

  FD_LOG_NOTICE(( "Shutting down..." ));
  FD_LOG_NOTICE(( "  Updates received: %lu", update_cnt ));
  FD_LOG_NOTICE(( "  Slots seen:       %lu", slot_cnt ));
  FD_LOG_NOTICE(( "  Total received:   %lu", received ));
  FD_LOG_NOTICE(( "  Total overruns:   %lu", overruns ));

  /* Detach from workspace */
  fd_accstr_consumer_detach( consumer );

  fd_halt();
  return 0;
}
