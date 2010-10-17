#ifndef __CCID_H__
#define __CCID_H__

#include "qdev.h"

typedef struct CCIDCardState CCIDCardState;
typedef struct CCIDCardInfo CCIDCardInfo;

struct CCIDCardState {
    DeviceState qdev;
};

struct CCIDCardInfo {
    DeviceInfo qdev;
    void (*print)(Monitor *mon, CCIDCardState *card, int indent);
    const uint8_t *(*get_atr)(CCIDCardState *card, uint32_t *len);
    void (*apdu_from_guest)(CCIDCardState *card, const uint8_t *apdu, uint32_t len);
    int (*exitfn)(CCIDCardState *card);
    int (*initfn)(CCIDCardState *card);
};

void ccid_card_send_apdu_to_guest(CCIDCardState *card, uint8_t* apdu, uint32_t len);
void ccid_card_card_removed(CCIDCardState *card);
void ccid_card_card_inserted(CCIDCardState *card);
void ccid_card_card_error(CCIDCardState *card, uint64_t error);
void ccid_card_qdev_register(CCIDCardInfo *card);

/* support guest visible insertion/removal of ccid devices based on actual
 * devices connected/removed. Called by card implementation (passthru, local) */
int ccid_card_ccid_attach(CCIDCardState *card);
void ccid_card_ccid_detach(CCIDCardState *card);

#endif // __CCID_H__

