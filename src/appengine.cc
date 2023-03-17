#include "appengine.h"

bool operator&(const AppEngine::Apps& apps, const AppEngine::App& app) {
  return apps.end() != std::find(apps.begin(), apps.end(), app);
}
