#ifndef ROLLBACK_FACTORY_H_
#define ROLLBACK_FACTORY_H_

#include "libaktualizr/config.h"
#include "rollback.h"

class RollbackFactory {
 public:
  RollbackFactory() {}
  static std::unique_ptr<Rollback> makeRollback(RollbackMode mode);
};

#endif
