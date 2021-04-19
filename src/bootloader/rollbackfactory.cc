#include "rollbacks/exception.h"
#include "rollbacks/factory.h"
#include "rollbacks/fiovb.h"
#include "rollbacks/generic.h"
#include "rollbacks/masked.h"

std::unique_ptr<Rollback> RollbackFactory::makeRollback(RollbackMode mode) {
  switch (mode) {
    case RollbackMode::kBootloaderNone:
      return std::make_unique<Rollback>();
      break;
    case RollbackMode::kUbootGeneric:
      return std::make_unique<GenericRollback>();
      break;
    case RollbackMode::kUbootMasked:
      return std::make_unique<MaskedRollback>();
      break;
    case RollbackMode::kFioVB:
      return std::make_unique<FiovbRollback>();
      break;
    default:
      return std::make_unique<ExceptionRollback>();
  }
}
