/* BearSSL sources are fetched at an immutable commit by 0020-stage-bearssl.sh. */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)
#include "sweet_bearssl/src/symcipher/aes_ct_enc.c"
#endif
