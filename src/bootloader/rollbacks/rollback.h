#ifndef ROLLBACK_H_
#define ROLLBACK_H_

class Rollback {
 public:
  Rollback() {}
  virtual ~Rollback() {}
  virtual void setBootOK() {}
  virtual void updateNotify() {}
};

#endif
