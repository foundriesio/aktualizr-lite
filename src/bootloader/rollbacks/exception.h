#ifndef AKTUALIZR_LITE_EXCEPTION_ROLLBACK_H_
#define AKTUALIZR_LITE_EXCEPTION_ROLLBACK_H_

#include "rollback.h"
#include "utilities/exceptions.h"

class ExceptionRollback : public Rollback {
 public:
  ExceptionRollback() = default;
  void setBootOK() override { throw NotImplementedException(); }
  void updateNotify() override { throw NotImplementedException(); }
  void installNotify(const Uptane::Target& /*target*/) override { throw NotImplementedException(); }
};

#endif  // AKTUALIZR_LITE_EXCEPTION_ROLLBACK_H_
