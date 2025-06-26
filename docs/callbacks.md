# Callbacks

Aktualizr-lite provides the ability to run an executable at the following OTA operations:

* Before checking in — check-for-update-pre  return: none
* After checking in  — check-for-update-post return: OK or FAILED: reason
* Before a download  — download-pre          return: none
* After a download   — download-post         return: OK or FAILED: reason
* Before an install  — install-pre           return: none
* After an install   — install-post          return: NEEDS_COMPLETION, OK, or FAILED: reason
* After a reboot     — install-final-pre     return: none

A simple recipe is in [aktualizr-callback](https://github.com/foundriesio/meta-lmp/blob/main/meta-lmp-base/recipes-sota/aktualizr/aktualizr-callback_1.0.bb)_ and a sample script is in [callback-handler](https://github.com/foundriesio/meta-lmp/blob/main/meta-lmp-base/recipes-sota/aktualizr/aktualizr-callback/callback-handler).