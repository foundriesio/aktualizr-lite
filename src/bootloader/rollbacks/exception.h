#ifndef EXCEPTION_ROLLBACK_H_
#define EXCEPTION_ROLLBACK_H_

#include "rollback.h"
#include "utilities/exceptions.h"

class ExceptionRollback : public Rollback {
 public:
  ExceptionRollback() : Rollback() {}
  void setBootOK() { throw NotImplementedException(); }
  void updateNotify() { throw NotImplementedException(); }
};

#endif
