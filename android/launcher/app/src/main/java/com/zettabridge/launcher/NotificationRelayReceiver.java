package com.zettabridge.launcher;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

import com.zettabridge.core.ZBridge;

/**
 * Posts a translated guest's notification when the alarm it scheduled fires.
 *
 * <p>Runs in the main process, deliberately: it needs no plugin code, only the notification
 * builder that {@link ZBridge#notifyGuest} uses directly. The guest cannot do this itself - its
 * notification helper is plugin Java that predates channels and PendingIntent mutability flags.
 */
public final class NotificationRelayReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null) return;
        try {
            ZBridge.postGuestNotification(context,
                    intent.getIntExtra(ZBridge.EXTRA_ID, 0),
                    intent.getStringExtra(ZBridge.EXTRA_TITLE),
                    intent.getStringExtra(ZBridge.EXTRA_TEXT));
        } catch (Throwable t) {
            Diagnostics.report(context, "relay: cannot post a guest notification", t, true);
        }
    }
}
