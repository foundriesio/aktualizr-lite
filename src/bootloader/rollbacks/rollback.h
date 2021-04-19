#ifndef ROLLBACK_H_
#define ROLLBACK_H_

#include "libaktualizr/types.h"

class Rollback {
 public:
  Rollback() {}
  virtual ~Rollback() {}
  virtual void setBootOK() {}
  virtual void updateNotify() {}
  virtual void installNotify(const Uptane::Target& target) { (void)target; }
};

#endif
