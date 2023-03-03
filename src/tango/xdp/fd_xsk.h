#ifndef HEADER_fd_src_tango_xdp_fd_xsk_h
#define HEADER_fd_src_tango_xdp_fd_xsk_h

#if defined(__linux__) && FD_HAS_LIBBPF

/* fd_xsk manages an XSK file descriptor and provides RX/TX buffers.

   ### Background

   AF_XDP is a Linux API providing kernel-bypass networking in the form
   of shared memory ring buffers accessible from userspace.  The kernel
   redirects packets from/to these buffers with the appropriate XDP
   configuration (XDP_REDIRECT).  AF_XDP is hardware-agnostic and allows
   sharing a NIC with the Linux networking stack (unlike e.g. DPDK).
   This allows for deployment in existing, heterogeneous networks. An
   AF_XDP socket is called "XSK".  The shared memory region storing the
   packet data flowing through an XSK is called "UMEM".

   XDP (eXpress Data Path) is a framework for installing hooks in the
   form of eBPF programs at an early stage of packet processing (i.e.
   before tc and netfilter).  eBPF is user-deployable JIT-compiled
   bytecode that usually runs inside the kernel. Some hardware/driver
   combinations optionally allow offloading eBPF processing to NICs.
   This is not to be confused with other BPF-erived ISAs such as sBPF
   (Solana BPF).

     +--- Figure 1: AF_XDP RX Block Diagram -----------------+
     |                                                       |
     |   ┌─────┐  ┌────────┐  ┌─────┐ XDP_PASS ┌─────────┐   |
     |   │ NIC ├──> Driver ├──> XDP ├──────────> sk_buff │   |
     |   └─────┘  └────────┘  └─┬───┘          └─────────┘   |
     |                          │                            |
     |                          │ XDP_REDIRECT               |
     |                          │                            |
     |                       ┌──▼───────┐      ┌─────────┐   |
     |                       │ XSK/UMEM ├──────> fd_aio  │   |
     |                       └──────────┘      └─────────┘   |
     |                                                       |
     +-------------------------------------------------------+

   Figure 1 shows a simplified block diagram of RX packet flow within
   the kernel in `XDP_FLAGS_DRV_MODE` mode.  Notably, the chain of eBPF
   programs installed in the XDP facility get invoked for every incoming
   packet.  If all programs return the `XDP_PASS` action, the packet
   continues its usual path to the Linux networking stack, where it will
   be allocated in sk_buff, and eventually flow through ip_rcv(), tc,
   and netfilter before reaching downstream sockets.
   If the `XDP_REDIRECT` action is taken however, the packet is copied
   to the UMEM of an XSK, and a RX queue entry is allocated.  An fd_aio
   backend is provided by fd_xdp_aio.
   The more generic `XDP_FLAGS_SKB_MODE` XDP mode falls back to sk_buff-
   based memory mgmt (still skipping the rest of the generic path), but
   is more widely available.

     +--- Figure 2: AF_XDP TX Block Diagram -------------+
     |                                                   |
     |   ┌────────┐  ┌──────────┐  ┌────────┐  ┌─────┐   |
     |   │ fd_aio ├──> XSK/UMEM ├──> Driver ├──> NIC │   |
     |   └────────┘  └──────────┘  └────────┘  └─────┘   |
     |                                                   |
     +---------------------------------------------------+

   Figure 2 shows a simplified block diagram of the TX packet flow.
   Userspace applications deliver packets to the XSK/UMEM buffers.  The
   kernel then forwards these packets to the NIC.  This also means that
   the application is responsible for maintaining a routing table to
   resolve layer-3 dest addrs to NICs and layer-2 addrs.  As in the RX
   flow, netfilter (iptables, nftables) is not available.

   ### Memory Management

   The UMEM area is allocated from userspace.  It is recommended to use
   the fd_util shmem/wksp APIs to obtain large page-backed memory.  UMEM
   is divided into equally sized frames. At any point in time, each
   frame is either owned by userspace or the kernel.  On initialization,
   all frames are owned by userspace.

   Changes in UMEM frame ownership and packet RX/TX events are
   transmitted via four rings allocated by the kernel (mmap()ed in by
   the user). This allows for out-of-order processing of packets.

      Data flow:
      (U->K) is userspace-to-kernel communication, and
      (K->U) is kernel-to-userspace.

      FILL         Free frames are provided to the kernel using the FILL
      (U->K)       ring. The kernel may populate these frames with RX
                   packet data.

      RX           Once the kernel has populated a FILL frame with RX
      (K->U)       packet data, it passes back the frame to userspace
                   via the RX queue.

      TX           TX frames sent by userspace are provided to the
      (U->K)       kernel using the TX ring.

      COMPLETION   Once the kernel has processed a TX frame, it passes
      (K->U)       back the frame to the userspace via the COMPLETION
                   queue.

   Combined, the FILL-RX and TX-COMPLETION rings form two pairs.  The
   kernel will not move frames between the pairs. */

#include <linux/if_link.h>
#include <net/if.h>

#include "../../util/fd_util_base.h"

/* Forward declarations */

struct fd_xsk_private;
typedef struct fd_xsk_private fd_xsk_t;

/* fd_xsk_frame_meta_t: Frame metadata used to identify packet */

#define FD_XDP_FRAME_META_ALIGN (16UL)

struct __attribute__((aligned(FD_XDP_FRAME_META_ALIGN))) fd_xsk_frame_meta {
  ulong off;   /* Offset to start of packet */
  uint  sz;    /* Size of packet data starting at `off` */
  uint  flags; /* Undefined for now */
};
typedef struct fd_xsk_frame_meta fd_xsk_frame_meta_t;

FD_PROTOTYPES_BEGIN

/* Setup API **********************************************************/

/* fd_xsk_{align,footprint} return the required alignment and
   footprint of a memory region suitable for use as an fd_xsk_t. */

FD_FN_CONST ulong
fd_xsk_align( void );

FD_FN_CONST ulong
fd_xsk_footprint( ulong frame_sz,
                  ulong fr_depth,
                  ulong rx_depth,
                  ulong tx_depth,
                  ulong cr_depth );

/* fd_xsk_new formats an unused memory region for use as an fd_xsk_t.
   shmem must point to a memory region that matches fd_xsk_align() and
   fd_xsk_footprint().  frame_sz controls the frame size used in the
   UMEM ring buffers.  {fr,rx,tx,cr}_depth control the number of frames
   allocated for the Fill, RX, TX, Completion rings respectively.
   Returns handle suitable for fd_xsk_join() on success. */

void *
fd_xsk_new( void * shmem,
            ulong  frame_sz,
            ulong  fr_depth,
            ulong  rx_depth,
            ulong  tx_depth,
            ulong  cr_depth );

/* fd_xsk_bind assigns an XSK buffer to the network device with name
   ifname and RX queue index ifqueue.  fd_xsk_unbind unassigns an XSK
   buffer from any netdev queue.  shxsk points to the first byte of the
   memory region backing the fd_xsk_t in the caller's address space.
   Returns shxsk on success or NULL on failure (logs details). */

void *
fd_xsk_bind( void *       shxsk,
             char const * ifname,
             uint         ifqueue );

void *
fd_xsk_unbind( void * shxsk );

/* fd_xsk_join joins the caller to the fd_xsk_t and starts packet
   redirection.  shxsk points to the first byte of the memory region
   backing the fd_xsk_t in the caller's address space.  Returns a
   pointer in the local address space to the fd_xsk on success or NULL
   on failure (logs details).  Reasons for failure include the shxsk is
   obviously not a local pointer to a memory region holding a xsk.
   Every successful join should have a matching leave.  The lifetime of
   the join is until the matching leave or caller's thread group is
   terminated.  There may only be one active join for a single fd_xsk_t
   at any given time.

   Requires prior system configuration via fd_xsk_install.  Creates an
   XSK, attaches the UMEM rings in fd_xsk_t with the XSK, updates the
   pre-installed XDP program's XSKMAP to start up packet redirection.
   Detaches any existing packet redirection on this XDP program. */

fd_xsk_t *
fd_xsk_join( void * shxsk );

/* fd_xsk_leave leaves a current local join and releases all kernel
   resources.  Returns a pointer to the underlying shared memory region
   on success and NULL on failure (logs details).  Reasons for failure
   include xsk is NULL. */

void *
fd_xsk_leave( fd_xsk_t * xsk );

/* fd_xsk_delete unformats a memory region used as an fd_xsk_t. Assumes
   nobody is joined to the region.  Returns a pointer to the underlying
   shared memory region or NULL if used obviously in error (e.g. shxsk
   does not point to an fd_xsk_t ... logs details).  The ownership of
   the memory region is transferred to the caller on success. */

void *
fd_xsk_delete( fd_xsk_t * xsk );

/* I/O API ************************************************************/

/* fd_xsk_tx_need_wakeup: returns whether a wakeup is required to
   complete a tx operation */

int
fd_xsk_tx_need_wakeup( fd_xsk_t * xsk );

/* fd_xsk_rx_need_wakeup: returns whether a wakeup is required to
   complete a rx operation */

int
fd_xsk_rx_need_wakeup( fd_xsk_t * xsk );

/* fd_xsk_rx_enqueue: Enqueues a batch of frames for RX.

   offsets_cnt is the no. of frames to attempt to enqueue for receive.
   offset/meta points to an array containing offsets_cnt items where
   each k in [0,count-1] attempts to enqueue frame at offset offset[k].
   Returns the number of frames actually enqueued, which may be less
   than offsets_cnt. Successful enqueue does not imply that packets have
   actually been received, but rather just indicates that the frame
   memory is registered with the AF_XDP socket.  The frames that failed
   to enqueue are referred to by offset[N+] and may be retried in a
   later call.

   fd_xsk_rx_enqueue and fd_xsk_rx_enqueue2 are the same, except
   fd_xsk_rx_enqueue2 takes fd_xsk_frame_meta_t* instead of ulong* and
   simply ignores the redundant info. */

ulong
fd_xsk_rx_enqueue( fd_xsk_t * xsk,
                   ulong *    offsets,
                   ulong      offsets_cnt );

ulong
fd_xsk_rx_enqueue2( fd_xsk_t *            xsk,
                    fd_xsk_frame_meta_t * meta,
                    ulong                 meta_cnt );


/* fd_xsk_tx_enqueue: Enqueues a batch of frames for TX.

   meta_cnt is the number of packets to attempt to enqueue for transmit.
   meta points to an array containing meta_cnt records where each k in
   [0,count-1] enqueues frame at meta[k].  Returns the number of frames
   actually enqueued, which may be less than meta_cnt.  Successful en-
   queue does not imply that packets have actually been sent out to the
   network, but rather just indicates that the frame memory is
   registered with the AF_XDP sockets.  The frames that failed to
   enqueue are referred to by meta[N+] and may be retried in a later
   call. */

ulong
fd_xsk_tx_enqueue( fd_xsk_t *            xsk,
                   fd_xsk_frame_meta_t * meta,
                   ulong                 meta_cnt );


/* fd_xsk_rx_complete: Receives RX completions for a batch of frames.

   meta_cnt is the number of packets that the caller is able to receive.
   meta points to an array containing meta_cnt records where each k in
   [0,count-1] may fill a packet meta at meta[k].  Returns the number of
   packets actually received, which may be less than meta_cnt. */

ulong
fd_xsk_rx_complete( fd_xsk_t *            xsk,
                    fd_xsk_frame_meta_t * meta,
                    ulong                 meta_cnt );


/* fd_xsk_tx_complete: Receives TX completions for a batch of frames.

   offsets_cnt/meta_cnt is the number of tx frame completions that the
   caller is able to receive. offsets/meta points to an array containing
   offsets_cnt/meta_cnt records where each k in [0,count-1] may write
   a completion at offsets[k] or meta[k].  Returns the number of packets
   that have been handed over to the NIC.  Note that this does not
   guarantee successful delivery to the network destination

   fd_xsk_tx_complete and fd_xsk_tx_complete2 are the same, except
   fd_xsk_tx_complete2 takes fd_xsk_frame_meta_t* instead of ulong* and
   simply ignores the redundant info. */

ulong
fd_xsk_tx_complete( fd_xsk_t * xsk,
                    ulong *    offsets,
                    ulong      offsets_cnt );

ulong
fd_xsk_tx_complete2( fd_xsk_t *            xsk,
                     fd_xsk_frame_meta_t * meta,
                     ulong                 meta_cnt );

/* fd_xsk_fd: Returns the XSK file descriptor. */

FD_FN_PURE int
fd_xsk_fd( fd_xsk_t * const xsk );

/* fd_xsk_ifname: Returns the network interface name of that the
   XSK is currently bound to.  Return value points to memory owned by
   `xsk` and is valid for the lifetime of the local join.  May return
   NULL if the XSK is not bound. */

FD_FN_PURE char const *
fd_xsk_ifname( fd_xsk_t * const xsk );

/* fd_xsk_ifidx: Returns the network interface index of that the
   XSK is currently bound to.  May return zero if the XSK is not bound. */

FD_FN_PURE uint
fd_xsk_ifidx( fd_xsk_t * const xsk );

/* fd_xsk_ifqueue: Returns the queue index that the XSK is currently
   bound to (a network interface can have multiple queues). U.B if
   fd_xsk_ifname() returns NULL. */

FD_FN_PURE uint
fd_xsk_ifqueue( fd_xsk_t * const xsk );

/* fd_xsk_umem_laddr returns a pointer to the XSK frame memory region in
   the caller's local address space. */

FD_FN_CONST void *
fd_xsk_umem_laddr( fd_xsk_t * xsk );

FD_PROTOTYPES_END

#endif /* defined(__linux__) && FD_HAS_LIBBPF */
#endif /* HEADER_fd_src_tango_xdp_fd_xsk_h */
