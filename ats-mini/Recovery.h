#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdint.h>

// Enter recovery mode menu. Called from setup() when encoder button is held.
// Does not return -- either reboots or enters a blocking USB/update loop.
void recoveryMode();

// Switch the OTA boot target to the standalone recovery partition (ota_2)
// and reboot into its boot menu. Does not return.
void bootRecoveryPartition();

#endif // RECOVERY_H
