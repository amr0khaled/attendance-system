#ifndef HOST_CHANNEL_H_
#define HOST_CHANNEL_H_

#include <flutter_linux/flutter_linux.h>

// Registers the "attendance/host" MethodChannel on the given messenger.
void host_channel_register(FlBinaryMessenger *messenger);

#endif // HOST_CHANNEL_H_
