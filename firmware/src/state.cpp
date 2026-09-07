#include "state.h"
#include <string.h>

RobotState g_state;

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Boot: return "boot";
        case Mode::Provision: return "provision";
        case Mode::Connecting: return "connecting";
        case Mode::Standby: return "standby";
        case Mode::Listening: return "listening";
        case Mode::Thinking: return "thinking";
        case Mode::Speaking: return "speaking";
        case Mode::Muted: return "muted";
        case Mode::Sleep: return "sleep";
    }
    return "?";
}

static const char* kExprNames[] = {"neutral", "happy", "sad", "surprised", "thinking", "sleepy",
                                   "angry", "love", "confused", "wink", "error"};

const char* expressionName(Expression e) { return kExprNames[(int)e]; }

bool expressionFromName(const char* n, Expression& out) {
    if (!n) return false;
    for (size_t i = 0; i < sizeof(kExprNames) / sizeof(kExprNames[0]); i++) {
        if (strcasecmp(n, kExprNames[i]) == 0) { out = (Expression)i; return true; }
    }
    return false;
}
