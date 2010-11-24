/* Virtual Smart Card protocol definition
 *
 * This protocol is between a host implementing a group of virtual smart card
 * reader, and a client implementing a virtual smart card, or passthrough to
 * a real card.
 *
 * The current implementation passes the raw APDU's from 7816 and additionally
 * contains messages to setup and teardown readers, handle insertion and
 * removal of cards, negotiate the protocol and provide for error responses.
 *
 * Copyright (c) 2010 Red Hat.
 *
 * This code is licensed under the LGPL.
 */

#ifndef _VSCARD_COMMON_H
#define _VSCARD_COMMON_H

#include <stdint.h>

#define VERSION_MAJOR_BITS 11
#define VERSION_MIDDLE_BITS 11
#define VERSION_MINOR_BITS 10

#define MAKE_VERSION(major, middle, minor) \
     (  (major  << (VERSION_MINOR_BITS + VERSION_MIDDLE_BITS)) \
      | (middle <<  VERSION_MINOR_BITS) \
      | (minor)  )

/** IMPORTANT NOTE on VERSION
 *
 * The version below MUST be changed whenever a change in this file is made.
 *
 * The last digit, the minor, is for bug fix changes only.
 *
 * The middle digit is for backward / forward compatible changes, updates
 * to the existing messages, addition of fields.
 *
 * The major digit is for a breaking change of protocol, presumably
 * something that cannot be accomodated with the existing protocol.
 */

#define VSCARD_VERSION MAKE_VERSION(0,0,1)

#define VSCARD_UNDEFINED_READER_ID -1
#define VSCARD_MINIMAL_READER_ID    0

typedef enum {
    VSC_Init,
    VSC_Error,
    VSC_ReaderAdd,
    VSC_ReaderAddResponse,
    VSC_ReaderRemove,
    VSC_ATR,
    VSC_CardRemove,
    VSC_APDU,
    VSC_Reconnect
} VSCMsgType;

typedef enum {
    VSC_GENERAL_ERROR=1,
    VSC_CANNOT_ADD_MORE_READERS,
} VSCErrorCode;

typedef uint32_t reader_id_t;

typedef struct VSCMsgHeader {
    VSCMsgType type;
    reader_id_t   reader_id;
    uint32_t   length;
    uint8_t    data[0];
} VSCMsgHeader;

/* VSCMsgInit               Client <-> Host
 * Host replies with allocated reader id in ReaderAddResponse
 * */
typedef struct VSCMsgInit {
    uint32_t   version;
} VSCMsgInit;

/* VSCMsgError              Client <-> Host
 * */
typedef struct VSCMsgError {
    uint32_t   code;
} VSCMsgError;

/* VSCMsgReaderAdd          Client -> Host
 * Host replies with allocated reader id in ReaderAddResponse
 * name - name of the reader on client side.
 * */
typedef struct VSCMsgReaderAdd {
    uint8_t    name[0];
} VSCMsgReaderAdd;

/* VSCMsgReaderAddResponse  Host -> Client
 * Reply to ReaderAdd
 * */
typedef struct VSCMsgReaderAddResponse {
} VSCMsgReaderAddResponse;

/* VSCMsgReaderRemove       Client -> Host
 * */
typedef struct VSCMsgReaderRemove {
} VSCMsgReaderRemove;

/* VSCMsgATR                Client -> Host
 * Answer to reset. Sent for card insertion or card reset.
 * */
typedef struct VSCMsgATR {
    uint8_t     atr[0];
} VSCMsgATR;

/* VSCMsgCardRemove         Client -> Host
 * */
typedef struct VSCMsgCardRemove {
} VSCMsgCardRemove;

/* VSCMsgAPDU               Client <-> Host
 * */
typedef struct VSCMsgAPDU {
    uint8_t    data[0];
} VSCMsgAPDU;

/* VSCMsgReconnect          Host -> Client
 * */
typedef struct VSCMsgReconnect {
    uint32_t   ip;
    uint16_t   port;
} VSCMsgReconnect;

#endif
