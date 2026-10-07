#pragma once

#include "cels.h"

/**
 * Discrete event emitted by the StatusBadge child composable
 * that bubbles up the component tree to WindowContent.
 */
typedef struct BadgeNotificationEvent {
    int         badgeId;
    const char *message;
} BadgeNotificationEvent;

CEL_Lifecycle(BadgeLifecycle) {
    mount {
        cel_event(BadgeNotificationEvent, {
            .badgeId = 101,
            .message = "Badge mounted into window tree"
        });
    }
    unmount {}
}

/**
 * Child Composable demonstrating lifecycle mount notification.
 */
CEL_Composable(StatusBadge) {
    cel_lifecycle(BadgeLifecycle);
}
