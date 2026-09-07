#include "wakeword.h"
#include "mww.h"

namespace wakeword {
bool begin() {
    if (!mww::begin()) { log_w("wake: no on-device model; server-side wake clips will be used"); return false; }
    log_i("wake: microWakeWord '%s' active", mww::wakeWord());
    return true;
}
bool available() { return mww::available(); }
void feed(const int16_t* s, size_t n) { mww::feed(s, n); }
bool wasDetected() { return mww::wasDetected(); }
void setEnabled(bool on) { mww::setEnabled(on); }
}  // namespace wakeword
