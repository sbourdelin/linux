#ifndef __LINUX_TPM_COMMAND_H__
#define __LINUX_TPM_COMMAND_H__

/*
 * TPM Command constants from specifications at
 * http://www.trustedcomputinggroup.org
 */

/* Command TAGS */
#define TPM_TAG_RQU_COMMAND             193
#define TPM_TAG_RQU_AUTH1_COMMAND       194
#define TPM_TAG_RQU_AUTH2_COMMAND       195
#define TPM_TAG_RSP_COMMAND             196
#define TPM_TAG_RSP_AUTH1_COMMAND       197
#define TPM_TAG_RSP_AUTH2_COMMAND       198

/* Command Ordinals */
#define TPM_ORD_GETRANDOM               70
#define TPM_ORD_OSAP                    11
#define TPM_ORD_OIAP                    10
#define TPM_ORD_SEAL                    23
#define TPM_ORD_UNSEAL                  24
#define TPM_ORD_GET_CAP                101
#define TPM_ORD_STARTUP                153
#define TPM_ORD_CONTINUE_SELFTEST       83
#define TPM_ORD_PCRREAD                 21
#define TPM_ORD_PCREXTEND               20
#define TPM_ORD_SAVESTATE              152
#define TPM_ORD_READPUBEK              124

/* Other constants */
#define SRKHANDLE                       0x40000000
#define TPM_NONCE_SIZE                  20

#endif
