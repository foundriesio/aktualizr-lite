#ifndef AKTUALIZR_LITE_ROLLBACK_FACTORY_H_
#define AKTUALIZR_LITE_ROLLBACK_FACTORY_H_

#include "libaktualizr/config.h"
#include "rollback.h"

class RollbackFactory {
 public:
  static std::unique_ptr<Rollback> makeRollback(RollbackMode mode);
};

#endif  // AKTUALIZR_LITE_ROLLBACK_FACTORY_H_
