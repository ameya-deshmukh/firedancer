#ifndef HEADER_fd_src_tango_xdp_fd_xsk_private_h
#define HEADER_fd_src_tango_xdp_fd_xsk_private_h

#if defined(__linux__) && FD_HAS_LIBBPF

#include "fd_xsk.h"
#include <linux/if_xdp.h>

/* FD_XSK_ALIGN: alignment of fd_xsk_t. */
#define FD_XSK_ALIGN      (  32UL)

/* FD_XSK_UMEM_ALIGN: byte alignment of UMEM area within fd_xsk_t. */
#define FD_XSK_UMEM_ALIGN (4096UL)

/* fd_ring_desc_t describes an XSK ring in the local address space.
   All pointers fall into kernel-managed XSK buffers that are valid
   during the lifetime of an fd_xsk_t join. */
struct fd_ring_desc {
  void *  mem;
  union {
    void *            ptr;         /* Opaque pointer */
    struct xdp_desc * packet_ring; /* For RX, TX rings */
    ulong *           frame_ring;  /* For FILL, COMPLETION rings */
  };
  ulong * flags;
  ulong   depth;  /* Number of entries in ring */
  ulong   map_sz; /* Size of memory mapping */
  ulong * prod;
  ulong * cons;
  ulong   cached_prod;
  ulong   cached_cons;
};
typedef struct fd_ring_desc fd_ring_desc_t;

/* Private definition of an fd_xsk_t */

#define FD_XSK_MAGIC (0xf17eda2c3778736bUL) /* firedancer hex(xsk) */

struct __attribute__((aligned(FD_XSK_ALIGN))) fd_xsk_private {
  ulong magic;   /* ==FD_XSK_MAGIC */
  ulong session; /* TODO, use as mutex lock */

  /* Network interface config *****************************************/

  /* if_name_cstr: Name of network interface.
     if_id:        Index of network interface (volatile).
     if_queue_id:  Queue ID of interface that XSK binds to.

     Note: fd_xsk_t lives in shm, so technically, it cannot outlive an
     if_idx, whose lifetime is the system uptime.  We save the interface
     name regardless, which is less likely to change on modern kernels. */
  char if_name_cstr[ IF_NAMESIZE ];
  uint if_idx;
  uint if_queue_id;

  /* frame_sz: Controls the frame size used in the UMEM ring buffers. */
  ulong frame_sz;

  /* umem_sz: Total size of XSK ring shared memory area (contiguous).
     Aligned by FD_XSK_ALIGN. */
  ulong umem_sz;

  /* {fr,rx,tx,cr}_depth: Number of frames allocated for the Fill, RX,
    TX, Completion XSK rings respectively. */
  ulong fr_depth;
  ulong rx_depth;
  ulong tx_depth;
  ulong cr_depth;

  /* xdp_mode: XDP processing mode.  Defined by <linux/if_link.h>

     Valid values:

       0                   kernel default mode
       XDP_FLAGS_SKB_MODE  sk_buff generic mode (hardware-agnostic)
       XDP_FLAGS_DRV_MODE  driver XDP (requires driver support)
       XDP_FLAGS_HW_MODE   hardware-accelerated XDP
                           (requires NIC and driver support) */
  ulong xdp_mode;

  /* Per-join kernel objects ******************************************/

  /* Kernel descriptor of UMEM in local address space */
  struct xdp_umem_reg umem;

  /* Kernel descriptor of XSK rings in local address space */
  struct xdp_mmap_offsets offsets;

  /* Open file descriptors */
  int xsk_fd;         /* AF_XDP socket file descriptor */
  int xdp_map_fd;     /* eBPF XSKMAP */
  int xdp_udp_map_fd; /* eBPF UDP map */

  /* ring_{rx,tx,fr,cr}: XSK ring descriptors */

  fd_ring_desc_t ring_rx;
  fd_ring_desc_t ring_tx;
  fd_ring_desc_t ring_fr;
  fd_ring_desc_t ring_cr;

  /* Variable-length data *********************************************/

  /* ... UMEM area follows ... */
};

FD_PROTOTYPES_BEGIN

/* fd_xsk_umem_area: Returns ptr to first byte of UMEM region within
   fd_xsk_t memory area. */
FD_FN_CONST static inline void *
fd_xsk_umem_area( fd_xsk_t * xdp ) {
  return (void *)( (uchar *)xdp + sizeof(fd_xsk_t) );
}

FD_PROTOTYPES_END

#endif /* defined(__linux__) && FD_HAS_LIBBPF */
#endif /* HEADER_fd_src_tango_xdp_fd_xsk_private_h */
