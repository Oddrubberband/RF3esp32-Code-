#pragma once

// Both USB test harnesses use independent radio power and the audited radio
// timing and buffered console. Only the Skyworks harness drives FEM GPIOs.
#define RF3_USB_RADIO_BENCH (RF3_AUTOMATIC_FEM || RF3_USB_EBYTE)
#if RF3_USB_EBYTE && (RF3_AUTOMATIC_FEM || RF3_HARDWARE_PROFILE_ID != 2)
#error "The EBYTE USB configuration requires the 4 MB devboard without an external FEM"
#endif
