#ifndef DEBUG_SERVER_H
#define DEBUG_SERVER_H

#if defined(DEBUG)
// Start a JSON-RPC over HTTP debug server on the given port.
// Spawns a background thread; non-blocking.
void debug_server_start(int port);
#endif

#endif
