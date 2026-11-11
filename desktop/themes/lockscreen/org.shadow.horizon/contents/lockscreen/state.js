// SPDX-License-Identifier: GPL-3.0-or-later
//
// Conversation state of the lock screen, shared by all screens.
//
// KScreenLocker loads LockScreen.qml once per screen into one QML engine and
// forwards every key press to all of them, so each screen sees the same
// password and the same Return key, and each receives the authenticator's
// signals. A ".pragma library" script exists once per engine; keeping the
// state here makes sure a prompt is answered exactly once — a second answer
// would be queued by the PAM worker and consumed by the NEXT prompt as a
// wrong password.
.pragma library

var awaiting = false      // PAM asked and waits for an answer
var pending = null        // answer typed before PAM asked
var handled = false       // the current prompt signal was already dealt with

// PAM asks for input. Called by every screen for the same prompt.
function prompted(authenticator) {
    if (handled)
        return
    handled = true
    if (pending !== null) {
        var answer = pending
        pending = null
        awaiting = false
        authenticator.respond(answer)
    } else {
        awaiting = true
    }
}

// The prompt signal has been delivered to every screen.
function promptDelivered() {
    handled = false
}

// The user pressed Return. Called by every screen for the same key press;
// returns true when this call passed the text on.
function submit(authenticator, text, idle) {
    if (awaiting) {
        awaiting = false
        authenticator.respond(text)
        return true
    }
    if (idle) {
        // No conversation yet (first key press, or PAM gave up): start one
        // and answer as soon as it asks.
        pending = text
        authenticator.startAuthenticating()
        return true
    }
    // An answer is already on its way to PAM.
    return false
}

// The conversation ended (failure, cancel).
function reset() {
    awaiting = false
    pending = null
    handled = false
}
