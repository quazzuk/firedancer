# Account stream module for streaming account updates to external processes
# via shared memory.

$(call add-hdrs,fd_accstr_proto.h)
$(call add-hdrs,fd_accstr_filter.h)
$(call add-hdrs,fd_accstr_ctx.h)
$(call add-hdrs,fd_accstr_consumer.h)
$(call add-hdrs,fd_accstr_setup.h)

$(call add-objs,fd_accstr_ctx,fd_disco)
$(call add-objs,fd_accstr_consumer,fd_disco)
$(call add-objs,fd_accstr_setup,fd_disco)

# Example consumer program
$(call make-bin,fd_accstr_example,fd_accstr_example,fd_disco fd_tango fd_util fd_ballet)

# Unit test
$(call make-unit-test,test_accstr,test_accstr,fd_disco fd_tango fd_util)
