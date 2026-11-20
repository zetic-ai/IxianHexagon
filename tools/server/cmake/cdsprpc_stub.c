// Link-time stand-in for Qualcomm's libcdsprpc.so.1 (FastRPC) when cross-compiling on a host that does not have it.
// libzetic_ixian_hexagon.so NEEDs these symbols; the executable itself never calls them. The real library on the
// IQ9 (/usr/lib/aarch64-linux-gnu/libcdsprpc.so.1) is used at run time, this stub is never installed or shipped.
#include <stddef.h>
#include <stdint.h>

#define STUB(ret, name, args) ret name args { return (ret) 0; }

STUB(int,   fastrpc_mmap,             (int domain, int fd, void * addr, int offset, size_t length, int flags))
STUB(int,   fastrpc_munmap,           (int domain, int fd, void * addr, size_t length))
STUB(int,   remote_handle64_open,     (const char * name, uint64_t * ph))
STUB(int,   remote_handle64_close,    (uint64_t h))
STUB(int,   remote_handle64_invoke,   (uint64_t h, uint32_t sc, void * pra))
STUB(int,   remote_handle64_control,  (uint64_t h, uint32_t req, void * data, uint32_t len))
STUB(int,   remote_handle_control,    (uint32_t req, void * data, uint32_t len))
STUB(int,   remote_session_control,   (uint32_t req, void * data, uint32_t len))
STUB(void *, rpcmem_alloc2,           (int heapid, uint32_t flags, size_t size))
STUB(int,   rpcmem_to_fd,             (void * po))
void rpcmem_free(void * po) { (void) po; }
