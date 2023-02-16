#include "fd_aio.h"

/* This definition generates one non-inline compiled copy of
   fd_aio_send.  The function body is defined in `fd_aio.h`. */
extern ulong
fd_aio_send( fd_aio_t *        aio,
             fd_aio_buffer_t * batch,
             ulong             batch_sz );
